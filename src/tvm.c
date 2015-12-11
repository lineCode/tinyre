
#include "tutils.h"
#include "tvm.h"
#include "tlexer.h"

_INLINE static
VMSnap* snap_dup(VMSnap* src) {
	VMSnap* snap = _new(VMSnap, 1);
	RunCache *rc, *rc_src;

	snap->codes = src->codes;
	snap->last_chrcode = src->last_chrcode;
	snap->last_pos = src->last_pos;
	snap->str_pos = src->str_pos;
	snap->mr = src->mr;
	snap->prev = src->prev;

	if (src->run_cache) {
		rc = snap->run_cache = _new(RunCache, 1);
		for (rc_src = src->run_cache; rc_src; rc_src = rc_src->prev) {
			rc->codes_cache = rc_src->codes_cache;
			rc->mr = rc_src->mr;
			rc->prev = rc_src->prev ? _new(RunCache, 1) : NULL;
		}
	} else {
		snap->run_cache = NULL;
	}

	return snap;
}

_INLINE static
void snap_free(VMSnap* snap) {
	RunCache *rc, *rc2;
	if (snap->run_cache) {
		for (rc = snap->run_cache; rc;) {
			rc2 = rc;
			rc = rc->prev;
			free(rc2);
		}
	}
	free(snap);
}

void vm_input_next(VMState* vms) {
	vms->snap->last_pos = vms->snap->str_pos;
	vms->snap->str_pos = utf8_decode(vms->snap->str_pos, &vms->snap->last_chrcode);
}

_INLINE static
int jump_one_cmp(VMState* vms) {
	int num;
	int ins = *vms->snap->codes;

	if (ins >= 1 && ins <= 5) {
		if (ins >= 3 && ins <= 4) {
			num = *(vms->snap->codes + 1);
			vms->snap->codes += (num * 2 + 2);
		} else {
			vms->snap->codes += 2;
		}
	}
	return 1;
}

_INLINE static
int do_ins_cmp(VMState* vms) {
	int char_code = *(vms->snap->codes + 1);

	TRE_DEBUG_PRINT("INS_CMP\n");

	if (char_code == vms->snap->last_chrcode) {
		return 2;
	}
	return 0;
}


_INLINE static bool
char_cmp_spe(int chrcode_re, int charcode) {
	switch (chrcode_re) {
		case '.':
			//if (flag & tre_pattern_dotall) return true;
			//else 
			if (charcode != '\n') return true;
			break;
		case 'd': if (isdigit(charcode)) return true; break;
		case 'D': if (!isdigit(charcode)) return true; break;
		case 'w': if (isalnum(charcode) || charcode == '_') return true; break;
		case 'W': if (!isalnum(charcode) || charcode != '_') return true; break;
		case 's': if (isspace(charcode)) return true; break;
		case 'S': if (!(isspace(charcode))) return true; break;
		default: if (chrcode_re == charcode) return true;
	}
	return false;
}

_INLINE static
int do_ins_cmp_spe(VMState* vms) {
	int char_code = *(vms->snap->codes + 1);

	TRE_DEBUG_PRINT("INS_CMP_SPE\n");

	if (char_cmp_spe(char_code, vms->snap->last_chrcode)) {
		return 2;
	}
	return 0;
}

_INLINE static
int do_ins_cmp_multi(VMState* vms) {
	int i;
	int _type, _code;
	bool match = false;
	int* data = vms->snap->codes + 1;
	int num = *data++;

	TRE_DEBUG_PRINT("INS_CMP_MULTI\n");

	for (i = 0; i < num; i++) {
		_type = *((int*)data + i * 2);
		_code = *((int*)data + i * 2 + 1);

		if (_type == TK_CHAR) {
			if (_code == vms->snap->last_chrcode) {
				match = true;
				break;
			}
		} else if (_type == TK_SPE_CHAR) {
			if (char_cmp_spe(_code, vms->snap->last_chrcode)) {
				match = true;
				break;
			}
		}
	}

	if (match) {
		return (num * 2 + 2);
	}

	return 0;
}

_INLINE static
int do_ins_cmp_group(VMState* vms) {
	RunCache *rc;
	int index = *(vms->snap->codes + 1);
	MatchGroup* g = vms->groups + index;

	TRE_DEBUG_PRINT("INS_CMP_GROUP\n");

	// new cache
	rc = _new(RunCache, 1);
	rc->codes_cache = vms->snap->codes;
	rc->prev = vms->snap->run_cache;
	rc->mr = vms->snap->mr;

	// load group code
	vms->snap->run_cache = rc;
	vms->snap->codes = g->codes;
	vms->snap->mr.enable = false;

	// set match result, value of head
	vms->match_results[index].tmp = vms->snap->last_pos;

	return 1;
}

_INLINE static
int do_ins_group_end(VMState* vms) {
	RunCache *rc;
	int index = *(vms->snap->codes + 1);

	TRE_DEBUG_PRINT("INS_GROUP_END\n");

	// load cache
	rc = vms->snap->run_cache;
	if (rc) {
		vms->snap->codes = rc->codes_cache;
		vms->snap->run_cache = rc->prev;
		vms->snap->mr = rc->mr;
		free(rc);
	}

	// set match result
	vms->match_results[index].head = vms->match_results[index].tmp;
	vms->match_results[index].tail = vms->snap->last_pos;

	// end if GROUP(0) matched
	// 2 is length of CMP_GROUP
	return (index == 0) ? -1 : 2;
}

_INLINE static
void save_snap(VMState* vms) {
	vms->snap->prev = snap_dup(vms->snap);
}

_INLINE static
int do_ins_checkpoint(VMState* vms) {
	int llimit = *(vms->snap->codes + 1);
	int rlimit = *(vms->snap->codes + 2);

	TRE_DEBUG_PRINT("INS_CHECK_POINT\n");

	vms->snap->codes += 3;

	// a{0,0}
	if (rlimit == 0) {
		return jump_one_cmp(vms);
	}

	// a{2,1}
	if (rlimit != -1 && llimit > rlimit) {
		// sre_constants.error: bad repeat interval
		return -2;
	}

	vms->snap->mr.enable = true;
	vms->snap->mr.llimit = llimit;
	vms->snap->mr.rlimit = rlimit;
	vms->snap->mr.cur_repeat = 0;

	// save snap when reach llimit
	if (llimit == 0) {
		save_snap(vms);
	}

	return 1;
}

int vm_step(VMState* vms) {
	int ret;
	int cur_ins = *vms->snap->codes;

	if (cur_ins >= ins_cmp && cur_ins <= ins_group_end) {
		// no greedy match

		if (cur_ins == ins_cmp) {
			ret = do_ins_cmp(vms);
		} else if (cur_ins == ins_cmp_spe) {
			ret = do_ins_cmp_spe(vms);
		} else if (cur_ins == ins_cmp_multi) {
			ret = do_ins_cmp_multi(vms);
		} else if (cur_ins == ins_cmp_group) {
			ret = do_ins_cmp_group(vms);
		} else if (cur_ins == ins_group_end) {
			ret = do_ins_group_end(vms);
		}

		if (ret) {
			if (cur_ins < ins_cmp_group) vm_input_next(vms);
			// CMP_GROUP is very different,
			// It's begin of match, and end at GROUP_END.
			// Other CMPs is both begin and end of one match.
			if (cur_ins != ins_cmp_group) {
				// match again when a? a+ a* a{x,y}
				if (vms->snap->mr.enable) {
					vms->snap->mr.cur_repeat++;
					save_snap(vms);
					if (vms->snap->mr.cur_repeat == vms->snap->mr.rlimit) {
						vms->snap->mr.enable = false;
						vms->snap->codes += ret;
					}
				} else {
					vms->snap->codes += ret;
				}
			}
		} else {
			// backtracking
			if (vms->snap->prev) {
				vms->snap = vms->snap->prev;
				vms->snap->mr.enable = false;
				ret = jump_one_cmp(vms);
			}
		}
	} else if (cur_ins == ins_check_point) {
		ret = do_ins_checkpoint(vms);
	} else if (cur_ins == ins_match_start) {
		// ^
		vms->snap->codes += 1;
		ret = 1;
	} else if (cur_ins == ins_match_end) {
		// $
		vms->snap->codes += 1;
		ret = 1;
	}

	return ret;
}

VMState* vm_init(tre_Pattern* groups_info, const char* input_str) {
	VMState* vms = _new(VMState, 1);
	vms->input_str = input_str;
	vms->group_num = groups_info->num;
	vms->groups = groups_info->groups;

	// init match results of groups
	vms->match_results = _new(tre_group, groups_info->num);
	memset(vms->match_results, 0, sizeof(tre_group) * groups_info->num);
	vms->match_results[0].tmp = input_str;

	// init first snap
	vms->snap = _new(VMSnap, 1);
	vms->snap->codes = groups_info->groups[0].codes;
	vms->snap->run_cache = NULL;
	vms->snap->str_pos = utf8_decode(input_str, &vms->snap->last_chrcode);
	vms->snap->last_pos = input_str;
	memset(&vms->snap->mr, 0, sizeof(MatchRepeat));
	vms->snap->prev = NULL;
	return vms;
}

tre_group* vm_exec(VMState* vms) {
	int ret = 1;
	while (ret > 0) {
		ret = vm_step(vms);
	}

	if (ret == -1) {
		return vms->match_results;
	}

	return NULL;
}