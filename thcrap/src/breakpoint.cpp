/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Breakpoint handling.
  */

#include "thcrap.h"

/// Functions
/// ---------
// Breakpoint hook function, implemented in assembly. A CALL to this function
// is written to every breakpoint's address.
extern "C" void bp_entry(void);

// Performs breakpoint lookup, invocation and stack adjustments. Returns the
// number of bytes the stack has to be moved downwards by breakpoint_entry().
extern "C" size_t __cdecl breakpoint_process(breakpoint_local_t *bp_local, size_t cave_addr, x86_reg_t *regs);
/// ---------

/// Constants
/// ---------
#define CALL_LEN (sizeof(void*) + 1)
#define x86_CALL_NEAR_REL32 0xE8
#define x86_JMP_NEAR_REL32 0xE9
#define x86_NOP 0x90
#define x86_INT3 0xCC
/// ---------

size_t json_immediate_value(json_t *val, x86_reg_t *regs)
{
	if (!val || json_is_null(val)) {
		return 0;
	}
	if (json_is_integer(val)) {
		return (size_t)json_integer_value(val);
	}
	else if (!json_is_string(val)) {
		log_func_printf("the expression must be either an integer or a string.\n");
		return 0;
	}
	const char *expr = json_string_value(val);
	size_t ret = 0;
	eval_expr(expr, '\0', &ret, regs, NULL);
	return ret;
}

size_t *json_pointer_value(json_t *val, x86_reg_t *regs)
{
	const char *expr = json_string_value(val);
	if (!expr || json_string_length(val) < 3) {
		return NULL;
	}

	// This function returns a pointer to an expression - that means the expression must resolve to something that can be pointed to.
	// We'll accept only 2 kind of expressions:
	// - A dereferencing (for example "[ebp-8]"), where we'll skip the top-level dereferencing. After all, ebp-8 points to [ebp-8].
	// - A register name, without anything else. In that case, we can return a pointer to the register in the x86_reg_t structure.
	size_t *ptr;
	const char *expr_end;

	ptr = reg(regs, expr, &expr_end);
	if (ptr && expr_end[0] == '\0') {
		return ptr;
	}
	else if (expr[0] == '[') {
		expr_end = eval_expr(expr + 1, ']', (size_t*)&ptr, regs, NULL);
		if (expr_end && expr_end[0] != '\0') {
			log_func_printf("Warning: leftover bytes after dereferencing: '%s'\n", expr_end);
		}
		return ptr;
	}
	log_func_printf("Error: called with something other than a register or a dereferencing.\n");
	return NULL;
}

size_t* json_register_pointer(json_t *val, x86_reg_t *regs)
{
	return json_string_length(val) >= 3 ? reg(regs, json_string_value(val), nullptr) : nullptr;
}

size_t* json_object_get_register(json_t *object, x86_reg_t *regs, const char *key)
{
	return json_register_pointer(json_object_get(object, key), regs);
}

size_t* json_object_get_pointer(json_t *object, x86_reg_t *regs, const char *key)
{
	return json_pointer_value(json_object_get(object, key), regs);
}

size_t json_object_get_immediate(json_t *object, x86_reg_t *regs, const char *key)
{
	return json_immediate_value(json_object_get(object, key), regs);
}

int breakpoint_cave_exec_flag(json_t *bp_info)
{
	return !json_is_false(json_object_get(bp_info, "cave_exec"));
}

size_t __cdecl breakpoint_process(breakpoint_local_t *bp, size_t cave_addr, x86_reg_t *regs)
{
	size_t esp_diff = 0;

	// POPAD ignores the ESP register, so we have to implement our own mechanism
	// to be able to manipulate it.
	size_t esp_prev = regs->esp;

	int cave_exec = bp->func(regs, bp->json_obj);
	if(cave_exec) {
		// Point return address to codecave.
		regs->retaddr = cave_addr;
	}
	if(esp_prev != regs->esp) {
		// ESP change requested.
		// Shift down the regs structure by the requested amount
		esp_diff = regs->esp - esp_prev;
		memmove((BYTE*)(regs) + esp_diff, regs, sizeof(x86_reg_t));
	}
	return esp_diff;
}

bool breakpoint_from_json(const char *name, json_t *in, breakpoint_local_t *out) {
	if (!json_is_object(in)) {
		log_printf("breakpoint %s: not an object\n", name);
		return false;
	}

	if (json_object_get_eval_bool_default(in, "ignore", false, JEVAL_DEFAULT)) {
		log_printf("breakpoint %s: ignored\n", name);
		return false;
	}

	size_t cavesize;
	switch (json_object_get_eval_int(in, "cavesize", &cavesize, JEVAL_STRICT)) {
		default:
			log_printf("ERROR: invalid json type for cavesize of breakpoint %s, must be integer or string\n", name);
			return false;
		case JEVAL_NULL_PTR:
			log_printf("breakpoint %s: no cavesize specified\n", name);
			return false;
		case JEVAL_SUCCESS:
			if (cavesize < CALL_LEN) {
				log_printf("breakpoint %s: cavesize too small to implement breakpoint\n", name);
				return false;
			}
	}

	hackpoint_addr_t* addrs = hackpoint_addrs_from_json(json_object_get(in, "addr"));
	if (!addrs) {
		// Ignore breakpoints no valid addrs
		// It usually means the breakpoint doesn't apply for this game or game version.
		return false;
	}

	out->name = strdup(name);
	out->cavesize = cavesize;
	out->json_obj = json_incref(in);
	out->func = nullptr;
	out->addr = addrs;

	return true;
}

static inline void __fastcall cave_fix(BYTE *cave, BYTE *bp_addr)
{
	/// Fix relative stuff
	/// ------------------

	// #1: Relative far call / jump at the very beginning
	if(cave[0] == x86_CALL_NEAR_REL32 || cave[0] == x86_JMP_NEAR_REL32) {
		size_t dist_old = *((size_t*)(cave + 1));
		size_t dist_new = dist_old + bp_addr - cave;

		*(size_t*)(cave + 1) = dist_new;

		log_printf("fixing rel.addr. 0x%p to 0x%p... \n", dist_old, dist_new);
	}
	/// ------------------
}

static bool __fastcall breakpoint_local_init(
	breakpoint_local_t *bp_local
) {

	const char *const key = bp_local->name;
	// Multi-slot support
	const char *const slot = strchr(key, '#');
	const size_t key_len = slot ? (size_t)(slot - key) : strlen(key);

	VLA(char, bp_key, strlen("BP_") + key_len + 1);
	if (strncmp(key, "codecave:", 9) != 0) {
		strcpy(bp_key, "BP_");
		strncat(bp_key, key, key_len);
	} else {
		strcpy(bp_key, key);
	}
	bp_local->func = (BreakpointFunc_t)func_get(bp_key);

	const bool func_found = (bool)bp_local->func;
	if (!func_found) {
		hackpoints_error_function_not_found(bp_key, 0);
	}
	VLA_FREE(bp_key);
	return func_found;
}

extern "C" {
	extern const BYTE bp_entry_end;
	extern const BYTE bp_entry_caveptr;
	extern const BYTE bp_entry_localptr;
	extern const BYTE bp_entry_callptr;
}

static const size_t bp_entry_size = &bp_entry_end - (uint8_t*)&bp_entry;
static const size_t bp_entry_cave = &bp_entry_caveptr + 1 - (uint8_t*)&bp_entry;
static const size_t bp_entry_local = &bp_entry_localptr + 1 - (uint8_t*)&bp_entry;
static const size_t bp_entry_call = &bp_entry_callptr + 1 - (uint8_t*)&bp_entry;

int breakpoints_apply(breakpoint_local_t *breakpoints, size_t bp_count, HMODULE hMod)
{
	if(!breakpoints || !bp_count) {
		log_printf("No breakpoints to set up.\n");
		return 0;
	}

	size_t sourcecaves_total_size = 0;
	size_t valid_breakpoint_count = 0;
	size_t total_valid_addrs = 0;

	VLA(size_t, breakpoint_total_size, bp_count);

	log_printf(
		"-------------------------\n"
		"Setting up breakpoints...\n"
		"-------------------------"
	);

	for (size_t i = 0; i < bp_count; ++i) {

		breakpoint_total_size[i] = 0;

		breakpoint_local_t *const cur = &breakpoints[i];

		log_printf("\n(%2d/%2d) %s... ", i + 1, bp_count, cur->name);

		if (!breakpoint_local_init(cur)) {
			// Not found message inside function
			continue;
		}

		size_t cavesize = cur->cavesize;

		size_t cur_valid_addrs = 0;

		size_t addr;
		for (hackpoint_addr_t* cur_addr = cur->addr;
			 eval_hackpoint_addr(cur_addr, &addr, hMod);
			 ++cur_addr) {

			if (!addr) {
				// NULL_ADDR
				continue;
			}

			log_printf("\nat 0x%p... ", addr);

			if (!VirtualCheckRegion((const void*)addr, cur->cavesize)) {
				cur_addr->type = INVALID_ADDR;
				log_printf("not enough source bytes, skipping... ", addr);
				continue;
			}
			++cur_valid_addrs;
			log_printf("OK");
			sourcecaves_total_size += AlignUpToMultipleOf2(cavesize + CALL_LEN, 16);
			//breakpoint_total_size[i] = AlignUpToMultipleOf2(cavesize + CALL_LEN, 16);
			//sourcecaves_total_size += breakpoint_total_size[i];
		}

		if (!cur_valid_addrs) {
			continue;
		}
		breakpoint_total_size[i] = AlignUpToMultipleOf2(cavesize + CALL_LEN, 16);
		total_valid_addrs += cur_valid_addrs;
		
		++valid_breakpoint_count;
	}

	if (!total_valid_addrs) {
		log_printf("No breakpoints to render.\n");
		return 0;
	}

	uint8_t *const cave_source = (BYTE*)VirtualAlloc(0, sourcecaves_total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	memset(cave_source, x86_INT3, sourcecaves_total_size);

	const size_t callcaves_total_size = total_valid_addrs * bp_entry_size;
	uint8_t *const cave_call = (BYTE*)VirtualAlloc(0, callcaves_total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	for (uint8_t *callcave_fill = cave_call, *const callcave_fill_end = cave_call + callcaves_total_size;
		 callcave_fill < callcave_fill_end;
		 callcave_fill += bp_entry_size) {
		memcpy(callcave_fill, &bp_entry, bp_entry_size);
	}

	log_printf(
		"\n-------------------------\n"
		"Rendering breakpoints... (source cave at 0x%p, call cave at 0x%p)\n"
		"-------------------------\n",
		cave_source, cave_call
	);

	BYTE *sourcecave_p = cave_source;
	BYTE *callcave_p = cave_call;

	size_t current_asm_buf_size = BINHACK_BUFSIZE_MIN;
	BYTE* asm_buf = (BYTE*)malloc(BINHACK_BUFSIZE_MIN);
	// CALL bp_entry
	asm_buf[0] = x86_CALL_NEAR_REL32;
	if constexpr (BINHACK_BUFSIZE_MIN > CALL_LEN) {
		memset(asm_buf + CALL_LEN, x86_NOP, BINHACK_BUFSIZE_MIN - CALL_LEN);
	}

#define PatchBPEntryInst(bp_entry_instance, bp_entry_offset, type, value) \
{\
	type *const bp_instance_ptr = (type*const)((size_t)(bp_entry_instance) + (size_t)(bp_entry_offset));\
	*bp_instance_ptr = (type)(value);\
}

	for (size_t i = 0; i < bp_count; ++i) {
		if (breakpoint_total_size[i]) {
			const breakpoint_local_t *const cur = &breakpoints[i];

			const size_t cavesize = cur->cavesize;

			if (cavesize > current_asm_buf_size) {
				asm_buf = (BYTE*)realloc(asm_buf, cavesize);
				memset(asm_buf + current_asm_buf_size, x86_NOP, current_asm_buf_size - cavesize);
				current_asm_buf_size = cavesize;
			}

			size_t addr;
			for (hackpoint_addr_t* cur_addr = cur->addr;
				 eval_hackpoint_addr(cur_addr, &addr, hMod);
				 ++cur_addr) {

				if (!addr) {
					// NULL_ADDR
					continue;
				}

				PatchBPEntryInst(callcave_p, bp_entry_cave, size_t, sourcecave_p);
				PatchBPEntryInst(callcave_p, bp_entry_local, const breakpoint_local_t*, cur);
				PatchBPEntryInst(callcave_p, bp_entry_call, size_t, (size_t)&breakpoint_process - (size_t)bp_instance_ptr - sizeof(void*));

				// CALL bp_entry
				const size_t bp_dist = (size_t)callcave_p - (addr + CALL_LEN);
				*(size_t*)&asm_buf[1] = bp_dist;

				callcave_p += bp_entry_size;

				/// Cave assembly
				// Copy old code to cave
				memcpy(sourcecave_p, (void*)addr, cavesize);
				cave_fix(sourcecave_p, (BYTE*)addr);

				// JMP addr
				const size_t cave_dist = addr - ((size_t)sourcecave_p + CALL_LEN);
				sourcecave_p[cavesize] = x86_JMP_NEAR_REL32;
				*(size_t*)&sourcecave_p[cavesize + 1] = cave_dist;

				PatchRegion((void*)addr, NULL, asm_buf, cavesize);
				sourcecave_p += breakpoint_total_size[i];
			}
		}
	}
	free(asm_buf);
	VLA_FREE(breakpoint_total_size);

	DWORD idgaf;
	VirtualProtect(cave_source, sourcecaves_total_size, PAGE_EXECUTE, &idgaf);
	VirtualProtect(cave_call, callcaves_total_size, PAGE_EXECUTE, &idgaf);

	return bp_count - valid_breakpoint_count;
}
