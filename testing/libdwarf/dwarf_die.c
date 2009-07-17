/*-
 * Copyright (c) 2009 Kai Wang
 * Copyright (c) 2007 John Birrell (jb@freebsd.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "_libdwarf.h"

int
dwarf_child(Dwarf_Die die, Dwarf_Die *ret_die, Dwarf_Error *error)
{
	Dwarf_Die next;

	if (die == NULL || ret_die == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	if ((next = STAILQ_NEXT(die, die_next)) == NULL ||
	    next->die_level != die->die_level + 1) {
		*ret_die = NULL;
		DWARF_SET_ERROR(error, DWARF_E_NO_ENTRY);
		return (DW_DLV_NO_ENTRY);
	} else
		*ret_die = next;

	return (DW_DLV_OK);
}

int
dwarf_siblingof(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Die *caller_ret_die,
    Dwarf_Error *error)
{
	Dwarf_Die next;
	Dwarf_CU cu;
	int ret;

	if (dbg == NULL || caller_ret_die == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	if ((cu = dbg->dbg_cu_current) == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_CU_CURRENT);
		return (DW_DLV_ERROR);
	}

	ret = DW_DLV_OK;
	if (die == NULL) {
		*caller_ret_die = STAILQ_FIRST(&cu->cu_die);
		if (*caller_ret_die == NULL) {
			DWARF_SET_ERROR(error, DWARF_E_NO_ENTRY);
			ret = DW_DLV_NO_ENTRY;
		}
	} else {
		next = die;
		while ((next = STAILQ_NEXT(next, die_next)) != NULL) {
			if (next->die_level < die->die_level) {
				next = NULL;
				break;
			}
			if (next->die_level == die->die_level) {
				*caller_ret_die = next;
				break;
			}
		}
		if (next == NULL) {
			*caller_ret_die = NULL;
			DWARF_SET_ERROR(error, DWARF_E_NO_ENTRY);
			ret = DW_DLV_NO_ENTRY;
		}
	}

	return (ret);
}

int
dwarf_offdie(Dwarf_Debug dbg, Dwarf_Off offset, Dwarf_Die *caller_ret_die,
    Dwarf_Error *error)
{
	Dwarf_CU cu;
	Dwarf_Die die;

	if (dbg == NULL || caller_ret_die == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	STAILQ_FOREACH(cu, &dbg->dbg_cu, cu_next) {
		STAILQ_FOREACH(die, &cu->cu_die, die_next)
			if (die->die_offset == (uint64_t)offset) {
				*caller_ret_die = die;
				return (DW_DLV_OK);
			}
	}

	return (DW_DLV_NO_ENTRY);
}

int
dwarf_tag(Dwarf_Die die, Dwarf_Half *tag, Dwarf_Error *error)
{
	Dwarf_Abbrev ab;

	if (die == NULL || tag == NULL || (ab = die->die_ab) == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	*tag = (Dwarf_Half) ab->ab_tag;

	return (DW_DLV_OK);
}

int
dwarf_dieoffset(Dwarf_Die die, Dwarf_Off *ret_offset, Dwarf_Error *error)
{

	if (die == NULL || ret_offset == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	*ret_offset = die->die_offset;

	return (DW_DLV_OK);
}

int
dwarf_die_CU_offset(Dwarf_Die die, Dwarf_Off *ret_offset, Dwarf_Error *error)
{
	Dwarf_CU cu;

	if (die == NULL || ret_offset == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	cu = die->die_cu;
	assert(cu != NULL);

	*ret_offset = die->die_offset - cu->cu_offset;

	return (DW_DLV_OK);
}

int
dwarf_die_CU_offset_range(Dwarf_Die die, Dwarf_Off *cu_offset,
    Dwarf_Off *cu_length, Dwarf_Error *error)
{
	Dwarf_CU cu;

	if (die == NULL || cu_offset == NULL || cu_length == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	cu = die->die_cu;
	assert(cu != NULL);

	*cu_offset = cu->cu_offset;
	*cu_length = cu->cu_length;

	return (DW_DLV_OK);
}

extern char *anon_name;

int
dwarf_diename(Dwarf_Die die, const char **ret_name, Dwarf_Error *error)
{

	if (die == NULL || ret_name == NULL) {
		DWARF_SET_ERROR(error, DWARF_E_ARGUMENT);
		return (DW_DLV_ERROR);
	}

	if (die->die_name == NULL || !strcmp(die->die_name, anon_name)) {
		DWARF_SET_ERROR(error, DWARF_E_NO_ENTRY);
		return (DW_DLV_NO_ENTRY);
	}

	*ret_name = die->die_name;

	return (DW_DLV_OK);
}

int
dwarf_die_abbrev_code(Dwarf_Die die)
{

	assert(die != NULL);

	return (die->die_abnum);
}
