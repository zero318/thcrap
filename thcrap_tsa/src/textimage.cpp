/**
  * Touhou Community Reliant Automatic Patcher
  * Team Shanghai Alice support plugin
  *
  * ----
  *
  * Support for custom images in place of original (mostly text) sprites.
  *
  * Ctrl-F for `game_id` to find all places that have to be updated to support
  * a new game.
  */

#include <thcrap.h>
#include <unordered_map>
#include <vector>
#include <thtypes/internal/sprite.h>
#include "thcrap_tsa.h"
#include "textimage.hpp"

/// Errors
/// ======
#define SLOTSTR_ERROR "Must parse into a sprite slot or a known image file name"
/// ======

/// DirectX / D3DX types
/// ====================
// We don't care about any of these, but it's nice to have the correct types.
// Omitting the Direct3D version though, since this function is the same for
// both Direct3D 8 and 9.
typedef IUnknown IDirect3DDevice;
typedef IUnknown IDirect3DTexture;
typedef uint32_t D3DFORMAT;
typedef uint32_t D3DPOOL;
typedef uint32_t D3DRESOURCETYPE;
typedef uint32_t D3DMULTISAMPLE_TYPE;
typedef uint32_t D3DXIMAGE_FILEFORMAT;

typedef struct _D3DSURFACE_DESC {
	D3DFORMAT Format;
	D3DRESOURCETYPE Type;
	DWORD Usage;
	D3DPOOL Pool;
	UINT Size;
	D3DMULTISAMPLE_TYPE MultiSampleType;
	UINT Width;
	UINT Height;
} D3DSURFACE_DESC;

typedef struct _D3DXIMAGE_INFO {
	uint32_t Width;
	uint32_t Height;
	uint32_t Depth;
	uint32_t MipLevels;
	D3DFORMAT Format;
	D3DRESOURCETYPE ResourceType;
	D3DXIMAGE_FILEFORMAT ImageFileFormat;
} D3DXIMAGE_INFO;

typedef HRESULT __stdcall d3dtex_GetLevelDesc(
	IDirect3DTexture *that,
	UINT Level,
	D3DSURFACE_DESC *pDesc
);
/// ====================

/// Game memory pointers
/// ====================
HRESULT (__stdcall *D3DXCreateTextureFromFileInMemoryEx)(
	IDirect3DDevice *pDevice,
	const void *pSrcData,
	uint32_t SrcData,
	uint32_t Width,
	uint32_t Height,
	uint32_t MipLevels,
	uint32_t Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	uint32_t Filter,
	uint32_t MipFilter,
	D3DCOLOR ColorKey,
	D3DXIMAGE_INFO* pSrcInfo,
	PALETTEENTRY* pPalette,
	IDirect3DTexture** ppTexture
);
IDirect3DDevice* pD3DDevice;
IDirect3DTexture** TextureSlots;
char* SpriteSpecs;
void** SpriteScripts;
bool Initialized;
/// ====================

/// Text image info
/// ===============
struct textimage_t {
	/// Configuration
	/// =============
	const char *fn; // Will point into the runconfig
	unsigned int priority;
	uint32_t texture_slot;
	uint32_t sprite_slot;
	uint32_t sprite_w;
	uint32_t sprite_h;
	unsigned char *script_buf;
	/// =============

	/// Runtime data
	/// ============
	int sprite_count = 0;
	// Sprite specs for all sprites on the text image, in the game's own
	// format from thtypes' internal/sprite.h.
	char *specs = nullptr;

	// Used to fall back on an image with lower priority, if available
	textimage_t *lower = nullptr;
	textimage_t *higher = nullptr;
	/// =============

	HRESULT reload(bool fallback_on_failure);

	~textimage_t() {
		SAFE_FREE(specs);
		SAFE_DELETE_ARRAY(script_buf);
	}

	// Parses a JSON text image declaration into an instance of this class.
	static textimage_t* create(
		const char *slotstr,
		uint32_t sprite_slot,
		unsigned int priority,
		json_t *desc,
		textimage_t *lower
	);
};
/// ===============

/// Runtime data for a certain sprite slot
/// ======================================
struct sprite_runtime_t
{
	uint32_t sprite_slot;

	textimage_t *active_ti = nullptr;

	// The text image sprite we are meant to show.
	// Could only become available later, due to repatching.
	int active_sprite_on_ti = -1;

	// false: Game still has its own sprite, [spec_game] and
	//        [script_game] may be uninitialized
	//  true: We set up a sprite from a text image, [spec_game] and
	//        [script_game] contain backups of the original data
	bool is_ours = false;

	bool in_group = false;

	// The game's own sprite spec
	union {
		sprite_spec06_t th06;
	} spec_game;

	// Pointer to the game's own script for its original sprite
	void *script_game = nullptr;

	bool would_be_replaced();
};
/// ======================================

/// Our global data
/// ===============
// Lists all registered text images.
std::vector<textimage_t *> Images;

// Maps sprite slots to their runtime data.
std::unordered_map<uint32_t, sprite_runtime_t> SpriteRuntimeMap;

typedef sprite_runtime_t* sprite_group_t;

// All currently registered groups.
std::vector<sprite_group_t *> Groups;

void groups_clear()
{
	for(auto group : Groups) {
		SAFE_DELETE_ARRAY(group);
	}
	Groups.clear();
}
/// ===============

// Also used to determine supported games.
size_t sprite_spec_size()
{
	switch(game_id) {
	case TH06:  return sizeof(sprite_spec06_t);
	case TH07:  return sizeof(sprite_spec07_t);
	default:    return 0;
	}
}
/// -----------------------------

textimage_t* textimage_error(const char *text, ...)
{
	va_list va;
	va_start(va, text);
	log_vmboxf("Text image error", MB_ICONERROR, text, va);
	va_end(va);
	return nullptr;
}

bool sprite_runtime_t::would_be_replaced()
{
	if(!active_ti || active_sprite_on_ti < 0) {
		return false;
	} else if(active_sprite_on_ti >= active_ti->sprite_count) {
		log_printf(
			"(Text image) %s sprite %d not available or empty\n",
			active_ti->fn, active_sprite_on_ti
		);
		return false;
	}
	return true;
}

textimage_t* image_get(const char *fn, size_t *index = nullptr)
{
	for(size_t i = 0; i < Images.size(); i++) {
		if(!strcmp(Images[i]->fn, fn)) {
			if(index) {
				*index = i;
			}
			return Images[i];
		}
	}
	return nullptr;
}

textimage_t* image_next_on_top(int &i)
{
	while(++i < (int)Images.size()) {
		if(Images[i]->higher == nullptr) {
			return Images[i];
		}
	}
	return nullptr;
}

// [i] must be a signed integer.
#define image_foreach_on_top(i, image) \
	for(i = -1; (image = image_next_on_top(i)); )

// Applies the current state in the sprite runtime map to the game, taking
// sprite groups into account.
void textimage_commit()
{
	auto commit_sprite = [] (sprite_runtime_t *sr, bool set) {
		auto spec_size = sprite_spec_size();
		auto copy_size = spec_size;
		auto game_sprite_p = SpriteSpecs + (sr->sprite_slot * spec_size);
		auto game_script_p = SpriteScripts + sr->sprite_slot;

		switch(game_id) {
		// Don't overwrite the register order
		case TH07:
		case TH06:
			copy_size -= 4;
			break;
		default: // -Wswitch...
			break;
		}

		if(set) {
			if(!sr->is_ours) {
				memcpy(&sr->spec_game, game_sprite_p, copy_size);
				sr->script_game = *game_script_p;
			}
			auto our_sprite_p = sr->active_ti->specs + sr->active_sprite_on_ti * spec_size;
			memcpy(game_sprite_p, our_sprite_p, copy_size);
			if(sr->active_ti->script_buf) {
				*game_script_p = sr->active_ti->script_buf;
			}
		} else {
			if(sr->is_ours) {
				memcpy(game_sprite_p, &sr->spec_game, copy_size);
				*game_script_p = sr->script_game;
			}
		}
		sr->is_ours = set;
	};

	for(size_t i = 0; i < Groups.size(); i++) {
		auto &group = Groups[i];
		bool got_all_of_them = true;
		for(auto p = group; *p != nullptr; p++) {
			if(!(*p)->would_be_replaced()) {
				log_printf(
					"(Text image) Got no image for slot 0x%x, not activating group #%u\n",
					(*p)->sprite_slot, i
				);
				got_all_of_them = false;
				break;
			}
		}
		for(auto p = group; *p != nullptr; p++) {
			commit_sprite(*p, got_all_of_them);
		}
	}
	for(auto &srs : SpriteRuntimeMap) {
		if(!srs.second.in_group) {
			commit_sprite(&srs.second, srs.second.would_be_replaced());
		}
	}
}

HRESULT textimage_t::reload(bool fallback_on_failure)
{
	auto safe_release = [](IUnknown **pUnk) {
		if(*pUnk != nullptr) {
			auto refcount = (*pUnk)->Release();
			assert(refcount == 0);
			*pUnk = nullptr;
		}
	};

	IDirect3DTexture *tex = nullptr;

	auto &sr = SpriteRuntimeMap[sprite_slot];
	sr.sprite_slot = sprite_slot;
	if(sr.active_ti && sr.active_ti->priority > priority) {
		log_printf(
			"(Text image) Ignoring %s (lower priority than %s)\n",
			this->fn, sr.active_ti->fn
		);
		return -1;
	}

	if(!sr.active_ti && TextureSlots[texture_slot] != nullptr) {
		textimage_error("Texture slot %u is controlled by the game", texture_slot);
	}

	auto release_and_fallback = [&] (HRESULT ret) {
		safe_release(&tex);
		if(lower) {
			if(fallback_on_failure) {
				safe_release(&TextureSlots[texture_slot]);
				sr.active_ti = nullptr;
				log_printf("(Text image) Falling back to %s\n", lower->fn);
				return lower->reload(fallback_on_failure);
			}
		} else {
			safe_release(&TextureSlots[texture_slot]);
			sr.active_ti = nullptr;
		}
		return ret;
	};

	defer(textimage_commit());

	size_t image_size = 0;
	auto image_buf = stack_game_file_resolve(fn, &image_size);
	if(!image_buf) {
		return release_and_fallback(-1);
	}
	D3DXIMAGE_INFO srcinfo;
	HRESULT ret = D3DXCreateTextureFromFileInMemoryEx(
		pD3DDevice, image_buf, image_size,
		0, 0, 1, 0, 0, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0, &srcinfo, nullptr,
		&tex
	);
	free(image_buf);
	// Yes, invalid images are a valid fallback condition.
	if(FAILED(ret)) {
		return release_and_fallback(ret);
	}

	if(
		(srcinfo.Width) == 0
		|| (srcinfo.Height) == 0
		|| (srcinfo.Width % sprite_w) != 0
		|| (srcinfo.Height % sprite_h) != 0
	) {
		textimage_error("%s: "
			"Image size must be a multiple of %u\xC3\x97%u, got %u\xC3\x97%u",
			fn, sprite_w, sprite_h, srcinfo.Width, srcinfo.Height
		);
		return release_and_fallback(-2);
	}

	D3DSURFACE_DESC hw;
	auto GetLevelDesc = (d3dtex_GetLevelDesc *)(*(size_t**)tex)[14];
	if(FAILED(GetLevelDesc(tex, 0, &hw))) {
		// This will probably only ever give different values
		// on ancient 3dfx Voodoo cards anyway, so... :P
		hw.Width = srcinfo.Width;
		hw.Height = srcinfo.Height;
	}

	SAFE_FREE(specs);
	const int cols = (int)(srcinfo.Width / sprite_w);
	const int rows = (int)(srcinfo.Height / sprite_h);
	const auto sizeof_spec = sprite_spec_size();
	sprite_count = rows * cols;
	specs = (char *)malloc(sizeof_spec * sprite_count);

	int i = 0;
	int col = 0;
	int row = 0;

	auto spec_init_base = [&] (char *p) {
		auto s = (sprite_spec_base_t *)p;
		s->texture_slot = texture_slot;
		s->abs_w = (float)sprite_w;
		s->abs_h = (float)sprite_h;
		s->abs.left = (float)(col * s->abs_w);
		s->abs.top = (float)(row * s->abs_h);
		s->abs.right = s->abs.left + s->abs_w;
		s->abs.bottom = s->abs.top + s->abs_h;
		s->thtx_w = (float)srcinfo.Width;
		s->thtx_h = (float)srcinfo.Height;
		s->rel.left = s->abs.left / s->thtx_w;
		s->rel.top = s->abs.top / s->thtx_h;
		s->rel.right = s->abs.right / s->thtx_w;
		s->rel.bottom = s->abs.bottom / s->thtx_h;
	};

	auto *p = (char *)specs;
	while(i < sprite_count) {
		switch(game_id) {
		case TH07: {
			auto *p07 = (sprite_spec07_t*)p;
			p07->hw_texture_scale_w = (float)srcinfo.Width  / hw.Width;
			p07->hw_texture_scale_h = (float)srcinfo.Height / hw.Height;
		} // fallthrough
		case TH06:
			spec_init_base(p);
			break;
		default: // -Wswitch...
			break;
		}
		col++;
		if(col == cols) {
			col = 0;
			row++;
		}
		i++;
		p += sizeof_spec;
	}

	log_printf(
		"(Text image) Got %u sprites (%u rows \xC3\x97 %u columns)\n",
		rows * cols, rows, cols
	);
	safe_release(&TextureSlots[texture_slot]);
	TextureSlots[texture_slot] = tex;
	sr.active_ti = this;
	return 0;
}

textimage_t* textimage_t::create(
	const char *slotstr,
	uint32_t sprite_slot,
	unsigned int priority,
	json_t *desc,
	textimage_t *lower
)
{
	struct Option {
		bool valid;
		uint32_t val;
	};

	auto fn = json_object_get_string(desc, "filename");
	if(!fn) {
		return textimage_error(
			"%s[%u]: \"filename\" must be a string", slotstr, priority
		);
	}

	auto check_val = [desc, fn] (const char *val, uint32_t ge_than) {
		auto j = json_object_get(desc, val);
		auto v = json_integer_value(j);
		bool valid = json_is_integer(j) && v >= ge_than && v < UINT32_MAX;
		if(!valid) {
			textimage_error(
				"%s: \"%s\" must be an unsigned 32-bit integer%s",
				fn, val, ge_than >= 1 ? " greater than zero" : ""
			);
		}
		return Option{ valid, (uint32_t)v };
	};

	auto texture_slot = check_val("texture_slot", 0);
	auto sprite_w = check_val("sprite_w", 1);
	auto sprite_h = check_val("sprite_h", 1);

	if(!texture_slot.valid || !sprite_w.valid || !sprite_h.valid) {
		return nullptr;
	}

	auto script_j = json_object_get(desc, "script");
	unsigned char *script_buf = nullptr;
	if(script_j) {
		auto script_str = json_string_value(script_j);
		if(!script_str) {
			return textimage_error("%s: \"script\" should be a binary hack", fn);
		}
		auto script_len = binhack_calc_size(script_str);
		if(script_len == 0) {
			return textimage_error("%s: Error rendering \"script\" into binary", fn);
		}
		script_buf = new unsigned char[script_len];
		binhack_render(script_buf, 0, script_str);
	}
	auto ret = new textimage_t;
	ret->fn = fn;
	ret->priority = priority;
	ret->texture_slot = texture_slot.val;
	ret->sprite_slot = sprite_slot;
	ret->sprite_w = sprite_w.val;
	ret->sprite_h = sprite_h.val;
	ret->script_buf = script_buf;
	ret->lower = lower;
	if(lower) {
		lower->higher = ret;
	}
	return ret;
}

enum sprite_slot_type_t {
	SS_ERROR = 0,
	SS_SLOT, // Only [slot] valid
	SS_IMAGE // [slot] and [image] valid
};

struct sprite_slot_t {
	sprite_slot_type_t type = SS_ERROR;
	uint32_t slot;
	textimage_t *image = nullptr;

	static sprite_slot_t parse(const char *slotstr);
};

sprite_slot_t sprite_slot_t::parse(const char *slotstr)
{
	assert(slotstr);

	sprite_slot_t ret;
	str_address_ret_t sar;
	ret.slot = str_address_value(slotstr, nullptr, &sar);
	if(sar.error == STR_ADDRESS_ERROR_NONE) {
		ret.type = SS_SLOT;
		return ret;
	}

	ret.image = image_get(slotstr);
	if(ret.image) {
		ret.type = SS_IMAGE;
		ret.slot = ret.image->sprite_slot;
		return ret;
	}
	textimage_error("\"%s\": " SLOTSTR_ERROR, slotstr);
	return ret;
}

sprite_runtime_t* sprite_runtime_get(const char *slotstr)
{
	sprite_slot_t ss = sprite_slot_t::parse(slotstr);
	if(ss.type == SS_ERROR) {
		return nullptr;
	}
	auto sr = SpriteRuntimeMap.find(ss.slot);
	if(sr == SpriteRuntimeMap.end()) {
		return nullptr;
	} else if(ss.type == SS_IMAGE && sr->second.active_ti != ss.image) {
		return nullptr;
	}
	return &sr->second;
}

/// Breakpoints
/// ===========
int BP_textimage_init(x86_reg_t *regs, json_t *bp_info)
{
	if(sprite_spec_size() == 0) {
		textimage_error("Text images are not supported for this game.");
		return 1;
	}
	if(
		D3DXCreateTextureFromFileInMemoryEx ||
		pD3DDevice || TextureSlots || SpriteSpecs || SpriteScripts
	) {
		return 1;
	}
	// Parameters
	// ----------
	Initialized = true;
#define P(var) \
		*(size_t*)&var = json_object_get_immediate(bp_info, regs, #var); \
		if(var == nullptr) { \
			textimage_error("`textimage_init`: \"" #var "\" is zero"); \
			Initialized = false; \
		}

	P(D3DXCreateTextureFromFileInMemoryEx);
	P(pD3DDevice);
	P(TextureSlots);
	P(SpriteSpecs);
	P(SpriteScripts);
#undef P
	// ----------
	return BP_textimage_load(regs, bp_info);
}

int BP_textimage_load(x86_reg_t *regs, json_t *bp_info)
{
	if(!Initialized) {
		return 1;
	}
	// Parameters
	// ----------
	auto images = json_object_get(bp_info, "images");
	auto groups = json_object_get(bp_info, "groups");
	// ----------
	const char *slotstr;
	json_t *image_flarr;

	json_object_foreach(images, slotstr, image_flarr) {
		size_t priority;
		json_t *image_desc;
		textimage_t *image_last = nullptr;
		auto ss = sprite_slot_t::parse(slotstr);
		if(ss.type == SS_ERROR) {
			continue;
		}
		json_flex_array_foreach(image_flarr, priority, image_desc) {
			switch(json_typeof(image_desc)) {
			case JSON_OBJECT:
				ss.image = textimage_t::create(
					slotstr, ss.slot, priority, image_desc, image_last
				);
				if(ss.image) {
					size_t old_index;
					auto old = image_get(ss.image->fn, &old_index);
					if(old) {
						delete old;
						Images[old_index] = ss.image;
					} else {
						Images.push_back(ss.image);
					}
					ss.image->reload(false);
					image_last = ss.image;
				}
				break;
			case JSON_TRUE:
				if(ss.type == SS_IMAGE) {
					// Slotstring parsed as image filename, got a single image
					ss.image->reload(false);
				} else {
					// Slotstring parsed as slot, reload the image with the highest priority
					int i;
					textimage_t *image_cur;
					image_foreach_on_top(i, image_cur) {
						if(image_cur->sprite_slot == ss.slot) {
							image_cur->reload(true);
						}
					}
				}
				break;
			default:
				textimage_error(
					"%s[%u]: Must be either a JSON object to (re-)define this text image, or a JSON true to reload it",
					slotstr, priority
				);
				break;
			}
		}
	}

	[groups] {
		size_t group_num;
		json_t *group_j;
		if(!json_is_array(groups)) {
			return;
		}
		groups_clear();
		json_array_foreach(groups, group_num, group_j) {
			auto group_size = json_array_size(group_j);
			if(group_size < 2) {
				continue;
			}
			VLA(sprite_runtime_t *, slots, group_size);
			defer(VLA_FREE(slots));

			for(size_t i = 0; i < group_size; i++) {
				auto slotstr = json_array_get_string(group_j, i);
				if(!slotstr) {
					textimage_error("\"groups\"[%u][%u]: " SLOTSTR_ERROR, group_num, i);
					return;
				}
				auto sr = sprite_runtime_get(slotstr);
				if(!sr) {
					return;
				}
				sr->in_group = true;
				slots[i] = sr;
			}
			auto group_a = new sprite_group_t[group_size + 1];
			for(size_t i = 0; i < group_size; i++) {
				group_a[i] = slots[i];
			}
			group_a[group_size] = nullptr;
			Groups.push_back(group_a);
		}
	}();
	return 1;
}

int BP_textimage_set(x86_reg_t *regs, json_t *bp_info)
{
	if(!Initialized) {
		return 1;
	}
	// Parameters
	// ----------
	auto sprites = json_object_get(bp_info, "sprites");
	if(!json_is_object(sprites)) {
		textimage_error("`textimage_set`: "
			"\"sprites\" must be a JSON object mapping slotstrings to sprite numbers"
		);
		return 1;
	}
	// ----------
	const char *slotstr;
	json_t *val;
	size_t i = 0;
	json_object_foreach(sprites, slotstr, val) {
		auto sr = sprite_runtime_get(slotstr);
		if(sr) {
			sr->active_sprite_on_ti = (int)json_immediate_value(val, regs);
		}
	}
	textimage_commit();
	return 1;
}

int BP_textimage_is_active(x86_reg_t *regs, json_t *bp_info)
{
	if(!Initialized) {
		return 1;
	}
	// Parameters
	// ----------
	auto slots = json_object_get(bp_info, "slots");
	if(!json_is_array(slots) && !json_is_string(slots)) {
		textimage_error("`textimage_is_active`: "
			"\"slots\" must be a flexible array of slotstrings"
		);
		return 1;
	}
	// ----------
	size_t i;
	json_t *val;
	json_flex_array_foreach(slots, i, val) {
		auto sr = sprite_runtime_get(json_string_value(val));
		if(!sr || !sr->is_ours) {
			return 1;
		}
	}
	return 0;
}
/// ===========

void textimage_mod_repatch(json_t *files_changed)
{
	auto game = json_object_get(runconfig_get(), "game");
	auto game_str = json_string_value(game);
	auto game_len = json_string_length(game);
	size_t check_basename_len = 0;

	for(auto image : Images) {
		auto this_len = strlen(image->fn) + 1;
		check_basename_len = max(check_basename_len, this_len);
	};

	VLA(char, check_fn, game_len + 1 + check_basename_len);
	char *check_basename = memcpy_advance_dst(
		memcpy_advance_dst(
			check_fn, game_str, game_len
		), "/", 1
	);

	for(auto image : Images) {
		strncpy(check_basename, image->fn, check_basename_len);

		const char *changed_fn;
		json_t *changed_val;
		json_object_foreach(files_changed, changed_fn, changed_val) {
			if(strstr(changed_fn, check_fn)) {
				image->reload(true);
			}
		}
	}
	VLA_FREE(check_fn);
}

void textimage_mod_exit()
{
	for(auto image : Images) {
		delete image;
	}
	Images.clear();
	SpriteRuntimeMap.clear();
	groups_clear();
}