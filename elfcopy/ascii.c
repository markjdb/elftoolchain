/*-
 * Copyright (c) 2010 Kai Wang
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

#include <sys/cdefs.h>
#include <sys/param.h>
#include <ctype.h>
#include <err.h>
#include <gelf.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "elfcopy.h"

ELFTC_VCSID("$Id$");

static void append_data(struct section *s, const void *buf, size_t sz);
static char hex_digit(uint8_t n);
static int hex_value(char x);
static void finalize_data_section(struct section *s);
static int ishexdigit(char x);
static void ihex_write(int ofd, int type, uint64_t addr, uint64_t num,
    const void *buf, size_t sz);
static void ihex_write_00(int ofd, uint64_t addr, const void *buf, size_t sz);
static void ihex_write_01(int ofd);
static void ihex_write_04(int ofd, uint16_t addr);
static void ihex_write_05(int ofd, uint64_t e_entry);
static struct section *new_data_section(struct elfcopy *ecp, int sec_index,
    uint64_t off, uint64_t addr);
static int read_num(const char *line, int *len, uint64_t *num, size_t sz,
    int *checksum);
static int srec_read(const char *line, char *type, uint64_t *addr,
    uint8_t *data, size_t *sz);
static void srec_write(int ofd, char type, uint64_t addr, const void *buf,
    size_t sz);
static void srec_write_symtab(int ofd, const char *ofn, Elf *e, Elf_Scn *scn,
    GElf_Shdr *sh);
static void srec_write_S0(int ofd, const char *ofn);
static void srec_write_Sd(int ofd, char dr, uint64_t addr, const void *buf,
    size_t sz, size_t rlen);
static void srec_write_Se(int ofd, uint64_t e_entry, int forceS3);
static void write_num(char *line, int *len, uint64_t num, size_t sz,
    int *checksum);

#define	_LINE_BUFSZ	1024
#define	_DATA_BUFSZ	256

/*
 * Convert ELF object to S-Record.
 */
void
create_srec(struct elfcopy *ecp, int ifd, int ofd, const char *ofn)
{
	Elf *e;
	Elf_Scn *scn;
	Elf_Data *d;
	GElf_Ehdr eh;
	GElf_Shdr sh;
	uint64_t max_addr;
	size_t rlen;
	int elferr, addr_sz;
	char dr;

	if ((e = elf_begin(ifd, ELF_C_READ, NULL)) == NULL)
		errx(EX_DATAERR, "elf_begin() failed: %s",
		    elf_errmsg(-1));

	/* Output a symbol table for `symbolsrec' target. */
	if (!strncmp(ecp->otgt, "symbolsrec", strlen("symbolsrec"))) {
		scn = NULL;
		while ((scn = elf_nextscn(e, scn)) != NULL) {
			if (gelf_getshdr(scn, &sh) == NULL) {
				warnx("gelf_getshdr failed: %s",
				    elf_errmsg(-1));
				(void) elf_errno();
				continue;
			}
			if (sh.sh_type != SHT_SYMTAB)
				continue;
			srec_write_symtab(ofd, ofn, e, scn, &sh);
			break;
		}
	}

	if (ecp->flags & SREC_FORCE_S3)
		dr = '3';
	else {
		/*
		 * Find maximum address size in the first iteration.
		 */
		max_addr = 0;
		scn = NULL;
		while ((scn = elf_nextscn(e, scn)) != NULL) {
			if (gelf_getshdr(scn, &sh) == NULL) {
				warnx("gelf_getshdr failed: %s",
				    elf_errmsg(-1));
				(void) elf_errno();
				continue;
			}
			if ((sh.sh_flags & SHF_ALLOC) == 0 ||
			    sh.sh_type == SHT_NOBITS ||
			    sh.sh_size == 0)
				continue;
			if ((uint64_t) sh.sh_addr > max_addr)
				max_addr = sh.sh_addr;
		}
		elferr = elf_errno();
		if (elferr != 0)
			warnx("elf_nextscn failed: %s", elf_errmsg(elferr));

		if (max_addr <= 0xFFFF)
			dr = '1';
		else if (max_addr <= 0xFFFFFF)
			dr = '2';
		else
			dr = '3';
	}

	if (ecp->flags & SREC_FORCE_LEN) {
		addr_sz = dr - '0' + 1;
		if (ecp->srec_len < 1)
			rlen = 1;
		else if (ecp->srec_len + addr_sz + 1 > 255)
			rlen = 255 - (addr_sz + 1);
		else
			rlen = ecp->srec_len;
	} else
		rlen = 16;

	/* Generate S0 record which contains the output filename. */
	srec_write_S0(ofd, ofn);

	/* Generate S{1,2,3} data records for section data. */
	scn = NULL;
	while ((scn = elf_nextscn(e, scn)) != NULL) {
		if (gelf_getshdr(scn, &sh) == NULL) {
			warnx("gelf_getshdr failed: %s", elf_errmsg(-1));
			(void) elf_errno();
			continue;
		}
		if ((sh.sh_flags & SHF_ALLOC) == 0 ||
		    sh.sh_type == SHT_NOBITS ||
		    sh.sh_size == 0)
			continue;
		if (sh.sh_addr > 0xFFFFFFFF) {
			warnx("address space too big for S-Record file");
			continue;
		}
		(void) elf_errno();
		if ((d = elf_getdata(scn, NULL)) == NULL) {
			elferr = elf_errno();
			if (elferr != 0)
				warnx("elf_getdata failed: %s", elf_errmsg(-1));
			continue;
		}
		if (d->d_buf == NULL || d->d_size == 0)
			continue;
		srec_write_Sd(ofd, dr, sh.sh_addr, d->d_buf, d->d_size, rlen);
	}
	elferr = elf_errno();
	if (elferr != 0)
		warnx("elf_nextscn failed: %s", elf_errmsg(elferr));

	/* Generate S{7,8,9} end of block recrod. */
	if (gelf_getehdr(e, &eh) == NULL)
		errx(EX_SOFTWARE, "gelf_getehdr() failed: %s",
		    elf_errmsg(-1));
	srec_write_Se(ofd, eh.e_entry, ecp->flags & SREC_FORCE_S3);
}

void
create_elf_from_srec(struct elfcopy *ecp, int ifd)
{
	char line[_LINE_BUFSZ];
	uint8_t data[_DATA_BUFSZ];
	GElf_Ehdr oeh;
	struct section *s, *sec, *sec_temp, *shtab;
	FILE *ifp;
	uint64_t addr, entry, off, sec_addr;
	size_t sz;
	int _ifd, first, sec_index;
	char type;

	/* Reset internal section list. */
	if (!TAILQ_EMPTY(&ecp->v_sec))
		TAILQ_FOREACH_SAFE(sec, &ecp->v_sec, sec_list, sec_temp) {
			TAILQ_REMOVE(&ecp->v_sec, sec, sec_list);
			free(sec);
		}

	if ((_ifd = dup(ifd)) < 0)
		err(EX_IOERR, "dup failed");
	if ((ifp = fdopen(_ifd, "r")) == NULL)
		err(EX_IOERR, "fdopen failed");

	/* Create EHDR for output .o file. */
	if (gelf_newehdr(ecp->eout, ecp->oec) == NULL)
		errx(EX_SOFTWARE, "gelf_newehdr failed: %s",
		    elf_errmsg(-1));
	if (gelf_getehdr(ecp->eout, &oeh) == NULL)
		errx(EX_SOFTWARE, "gelf_getehdr() failed: %s",
		    elf_errmsg(-1));

	/* Initialise e_ident fields. */
	oeh.e_ident[EI_CLASS] = ecp->oec;
	oeh.e_ident[EI_DATA] = ecp->oed;
	/*
	 * TODO: Set OSABI according to the OS platform where elfcopy(1)
	 * was build. (probably)
	 */
	oeh.e_ident[EI_OSABI] = ELFOSABI_NONE;
	oeh.e_machine = ecp->oem;
	oeh.e_type = ET_REL;
	oeh.e_entry = 0;

	ecp->flags |= RELOCATABLE;

	/* Create .shstrtab section */
	init_shstrtab(ecp);
	ecp->shstrtab->off = 0;

	/* Data sections are inserted after EHDR. */
	off = gelf_fsize(ecp->eout, ELF_T_EHDR, 1, EV_CURRENT);
	if (off == 0)
		errx(EX_SOFTWARE, "gelf_fsize() failed: %s", elf_errmsg(-1));

	/* Create data sections. */
	s = NULL;
	first = 1;
	sec_index = 1;
	sec_addr = entry = 0;
	while (fgets(line, _LINE_BUFSZ, ifp) != NULL) {
		if (line[0] == '\r' || line[0] == '\n')
			continue;
		if (line[0] != 'S' || line[1] < '0' || line[1] > '9') {
			warnx("Invalid srec record");
			continue;
		}
		if (srec_read(line, &type, &addr, data, &sz) < 0) {
			warnx("Invalid srec record or mismatched checksum");
			continue;
		}
		switch (type) {
		case '1':
		case '2':
		case '3':
			if (sz == 0)
				break;
			if (first || sec_addr != addr) {
				if (s != NULL)
					finalize_data_section(s);
				s = new_data_section(ecp, sec_index, off,
				    sec_addr);
				if (s == NULL) {
					warnx("new_data_section failed");
					break;
				}
				sec_index++;
				sec_addr = addr;
				first = 0;
			}
			append_data(s, data, sz);
			off += sz;
			sec_addr += sz;
			break;
		case '7':
		case '8':
		case '9':
			entry = addr;
			break;
		default:
			break;
		}
	}
	if (s != NULL)
		finalize_data_section(s);
	if (ferror(ifp))
		warn("fgets failed");
	fclose(ifp);

	/* Insert .shstrtab after data sections. */
	if ((ecp->shstrtab->os = elf_newscn(ecp->eout)) == NULL)
		errx(EX_SOFTWARE, "elf_newscn failed: %s",
		    elf_errmsg(-1));
	insert_to_sec_list(ecp, ecp->shstrtab, 1);

	/* Insert section header table here. */
	shtab = insert_shtab(ecp, 1);

	/* Set entry point. */
	oeh.e_entry = entry;

	/*
	 * Write the underlying ehdr. Note that it should be called
	 * before elf_setshstrndx() since it will overwrite e->e_shstrndx.
	 */
	if (gelf_update_ehdr(ecp->eout, &oeh) == 0)
		errx(EX_SOFTWARE, "gelf_update_ehdr() failed: %s",
		    elf_errmsg(-1));

	/* Generate section name string table (.shstrtab). */
	/* ecp->flags |= SYMTAB_EXIST; */
	set_shstrtab(ecp);

	/* Update sh_name pointer for each section header entry. */
	update_shdr(ecp);

	/* Renew oeh to get the updated e_shstrndx. */
	if (gelf_getehdr(ecp->eout, &oeh) == NULL)
		errx(EX_SOFTWARE, "gelf_getehdr() failed: %s",
		    elf_errmsg(-1));

	/* Resync section offsets. */
	resync_sections(ecp);

	/* Store SHDR offset in EHDR. */
	oeh.e_shoff = shtab->off;

	/* Update ehdr since we modified e_shoff. */
	if (gelf_update_ehdr(ecp->eout, &oeh) == 0)
		errx(EX_SOFTWARE, "gelf_update_ehdr() failed: %s",
		    elf_errmsg(-1));

	/* Write out the output elf object. */
	if (elf_update(ecp->eout, ELF_C_WRITE) < 0)
		errx(EX_SOFTWARE, "elf_update() failed: %s",
		    elf_errmsg(-1));
}

void
create_ihex(int ifd, int ofd)
{
	Elf *e;
	Elf_Scn *scn;
	Elf_Data *d;
	GElf_Ehdr eh;
	GElf_Shdr sh;
	int elferr;
	uint16_t addr_hi, old_addr_hi;

	if ((e = elf_begin(ifd, ELF_C_READ, NULL)) == NULL)
		errx(EX_DATAERR, "elf_begin() failed: %s",
		    elf_errmsg(-1));

	old_addr_hi = 0;
	scn = NULL;
	while ((scn = elf_nextscn(e, scn)) != NULL) {
		if (gelf_getshdr(scn, &sh) == NULL) {
			warnx("gelf_getshdr failed: %s", elf_errmsg(-1));
			(void) elf_errno();
			continue;
		}
		if ((sh.sh_flags & SHF_ALLOC) == 0 ||
		    sh.sh_type == SHT_NOBITS ||
		    sh.sh_size == 0)
			continue;
		if (sh.sh_addr > 0xFFFFFFFF) {
			warnx("address space too big for Intel Hex file");
			continue;
		}
		(void) elf_errno();
		if ((d = elf_getdata(scn, NULL)) == NULL) {
			elferr = elf_errno();
			if (elferr != 0)
				warnx("elf_getdata failed: %s", elf_errmsg(-1));
			continue;
		}
		if (d->d_buf == NULL || d->d_size == 0)
			continue;
		addr_hi = (sh.sh_addr >> 16) & 0xFFFF;
		if (addr_hi > 0 && addr_hi != old_addr_hi) {
			/* Write 04 record if addr_hi is new. */
			old_addr_hi = addr_hi;
			ihex_write_04(ofd, addr_hi);
		}
		ihex_write_00(ofd, sh.sh_addr, d->d_buf, d->d_size);
	}
	elferr = elf_errno();
	if (elferr != 0)
		warnx("elf_nextscn failed: %s", elf_errmsg(elferr));

	if (gelf_getehdr(e, &eh) == NULL)
		errx(EX_SOFTWARE, "gelf_getehdr() failed: %s",
		    elf_errmsg(-1));
	ihex_write_05(ofd, eh.e_entry);
	ihex_write_01(ofd);
}

#define	_SEC_NAMESZ	64
#define	_SEC_INIT_CAP	1024

static struct section *
new_data_section(struct elfcopy *ecp, int sec_index, uint64_t off,
    uint64_t addr)
{
	char *name;

	if ((name = malloc(_SEC_NAMESZ)) == NULL)
		errx(EX_SOFTWARE, "malloc failed");
	snprintf(name, _SEC_NAMESZ, ".sec%d", sec_index);

	return (create_external_section(ecp, name, NULL, 0, off, SHT_PROGBITS,
	    ELF_T_BYTE, 0, 1, addr, 0));
}

static void
finalize_data_section(struct section *s)
{
	Elf_Data *od;

	if ((od = elf_newdata(s->os)) == NULL)
		errx(EX_SOFTWARE, "elf_newdata() failed: %s",
		    elf_errmsg(-1));
	od->d_align = s->align;
	od->d_off = 0;
	od->d_buf = s->buf;
	od->d_size = s->sz;
	od->d_version = EV_CURRENT;
}

static void
append_data(struct section *s, const void *buf, size_t sz)
{
	uint8_t *p;

	if (s->buf == NULL) {
		s->sz = 0;
		s->cap = _SEC_INIT_CAP;
		if ((s->buf = malloc(s->cap)) == NULL)
			err(EX_SOFTWARE, "malloc failed");
	}

	while (sz + s->sz > s->cap) {
		s->cap *= 2;
		if ((s->buf = realloc(s->buf, s->cap)) == NULL)
			err(EX_SOFTWARE, "realloc failed");
	}

	p = s->buf;
	memcpy(&p[s->sz], buf, sz);
	s->sz += sz;
}

static int
srec_read(const char *line, char *type, uint64_t *addr, uint8_t *data,
    size_t *sz)
{
	uint64_t count, _checksum, num;
	size_t addr_sz;
	int checksum, i, len;

	checksum = 0;
	len = 2;
	if (read_num(line, &len, &count, 1, &checksum) < 0)
		return (-1);
	*type = line[1];
	switch (*type) {
	case '0':
	case '1':
	case '5':
	case '9':
		addr_sz = 2;
		break;
	case '2':
	case '8':
		addr_sz = 3;
		break;
	case '3':
	case '7':
		addr_sz = 4;
		break;
	default:
		return (-1);
	}

	if (read_num(line, &len, addr, addr_sz, &checksum) < 0)
		return (-1);

	count -= addr_sz + 1;
	if (*type >= '0' && *type <= '3') {
		for (i = 0; (uint64_t) i < count; i++) {
			if (read_num(line, &len, &num, 1, &checksum) < 0)
				return -1;
			data[i] = (uint8_t) num;
		}
		*sz = count;
	} else
		*sz = 0;

	if (read_num(line, &len, &_checksum, 1, NULL) < 0)
		return (-1);

	if ((int) _checksum != (~checksum & 0xFF))
		return (-1);

	return (0);
}

static void
srec_write_symtab(int ofd, const char *ofn, Elf *e, Elf_Scn *scn, GElf_Shdr *sh)
{
	char line[_LINE_BUFSZ];
	GElf_Sym sym;
	Elf_Data *d;
	const char *name;
	size_t sc;
	int elferr, i;

#define _WRITE_LINE do {						\
	if (write(ofd, line, strlen(line)) != (ssize_t) strlen(line)) 	\
		errx(EX_IOERR, "write failed");				\
	} while (0)


	(void) elf_errno();
	if ((d = elf_getdata(scn, NULL)) == NULL) {
		elferr = elf_errno();
		if (elferr != 0)
			warnx("elf_getdata failed: %s",
			    elf_errmsg(-1));
		return;
	}
	if (d->d_buf == NULL || d->d_size == 0)
		return;

	snprintf(line, sizeof(line), "$$ %s\r\n", ofn);
	_WRITE_LINE;
	sc = d->d_size / sh->sh_entsize;
	for (i = 1; (size_t) i < sc; i++) {
		if (gelf_getsym(d, i, &sym) != &sym) {
			warnx("gelf_getsym failed: %s", elf_errmsg(-1));
			continue;
		}
		if (GELF_ST_TYPE(sym.st_info) == STT_SECTION ||
		    GELF_ST_TYPE(sym.st_info) == STT_FILE)
			continue;
		if ((name = elf_strptr(e, sh->sh_link, sym.st_name)) == NULL) {
			warnx("elf_strptr failed: %s", elf_errmsg(-1));
			continue;
		}
		snprintf(line, sizeof(line), "  %s $%jx\r\n", name,
		    (uintmax_t) sym.st_value);
		_WRITE_LINE;
	}
	snprintf(line, sizeof(line), "$$ \r\n");
	_WRITE_LINE;

#undef	_WRITE_LINE
}

static void
srec_write_S0(int ofd, const char *ofn)
{

	srec_write(ofd, '0', 0, ofn, strlen(ofn));
}

static void
srec_write_Sd(int ofd, char dr, uint64_t addr, const void *buf, size_t sz,
    size_t rlen)
{
	const uint8_t *p, *pe;

	p = buf;
	pe = p + sz;
	while (pe - p >= (int) rlen) {
		srec_write(ofd, dr, addr, p, rlen);
		addr += rlen;
		p += rlen;
	}
	if (pe - p > 0)
		srec_write(ofd, dr, addr, p, pe - p);
}

static void
srec_write_Se(int ofd, uint64_t e_entry, int forceS3)
{
	char er;

	if (e_entry > 0xFFFFFFFF) {
		warnx("address space too big for S-Record file");
		return;
	}

	if (forceS3)
		er = '7';
	else {
		if (e_entry <= 0xFFFF)
			er = '9';
		else if (e_entry <= 0xFFFFFF)
			er = '8';
		else
			er = '7';
	}

	srec_write(ofd, er, e_entry, NULL, 0);
}

static void
srec_write(int ofd, char type, uint64_t addr, const void *buf, size_t sz)
{
	char line[_LINE_BUFSZ];
	const uint8_t *p, *pe;
	int len, addr_sz, checksum;

	if (type == '0' || type == '1' || type == '5' || type == '9')
		addr_sz = 2;
	else if (type == '2' || type == '8')
		addr_sz = 3;
	else
		addr_sz = 4;

	checksum = 0;
	line[0] = 'S';
	line[1] = type;
	len = 2;
	write_num(line, &len, addr_sz + sz + 1, 1, &checksum);
	write_num(line, &len, addr, addr_sz, &checksum);
	for (p = buf, pe = p + sz; p < pe; p++)
		write_num(line, &len, *p, 1, &checksum);
	write_num(line, &len, ~checksum & 0xFF, 1, NULL);
	line[len++] = '\r';
	line[len++] = '\n';
	if (write(ofd, line, len) != (ssize_t) len)
		err(EX_DATAERR, "write failed");
}

static void
ihex_write_00(int ofd, uint64_t addr, const void *buf, size_t sz)
{
	uint16_t addr_hi, old_addr_hi;
	const uint8_t *p, *pe;

	old_addr_hi = (addr >> 16) & 0xFFFF;
	p = buf;
	pe = p + sz;
	while (pe - p >= 16) {
		ihex_write(ofd, 0, addr, 0, p, 16);
		addr += 16;
		p += 16;
		addr_hi = (addr >> 16) & 0xFFFF;
		if (addr_hi != old_addr_hi) {
			old_addr_hi = addr_hi;
			ihex_write_04(ofd, addr_hi);
		}
	}
	if (pe - p > 0)
		ihex_write(ofd, 0, addr, 0, p, pe - p);
}

static void
ihex_write_01(int ofd)
{

	ihex_write(ofd, 1, 0, 0, NULL, 0);
}

static void
ihex_write_04(int ofd, uint16_t addr)
{

	ihex_write(ofd, 4, 0, addr, NULL, 2);
}

static void
ihex_write_05(int ofd, uint64_t e_entry)
{

	if (e_entry > 0xFFFFFFFF) {
		warnx("address space too big for Intel Hex file");
		return;
	}

	ihex_write(ofd, 5, 0, e_entry, NULL, 4);
}

static void
ihex_write(int ofd, int type, uint64_t addr, uint64_t num, const void *buf,
    size_t sz)
{
	char line[_LINE_BUFSZ];
	const uint8_t *p, *pe;
	int len, checksum;

	if (sz > 16)
		errx(EX_SOFTWARE, "Internal: ihex_write() sz too big");
	checksum = 0;
	line[0] = ':';
	len = 1;
	write_num(line, &len, sz, 1, &checksum);
	write_num(line, &len, addr, 2, &checksum);
	write_num(line, &len, type, 1, &checksum);
	if (sz > 0) {
		if (buf != NULL) {
			for (p = buf, pe = p + sz; p < pe; p++)
				write_num(line, &len, *p, 1, &checksum);
		} else
			write_num(line, &len, num, sz, &checksum);
	}
	write_num(line, &len, (~checksum + 1) & 0xFF, 1, NULL);
	line[len++] = '\r';
	line[len++] = '\n';
	if (write(ofd, line, len) != (ssize_t) len)
		err(EX_DATAERR, "write failed");
}

static int
read_num(const char *line, int *len, uint64_t *num, size_t sz, int *checksum)
{
	uint8_t b;

	*num = 0;
	for (; sz > 0; sz--) {
		if (!ishexdigit(line[*len]) || !ishexdigit(line[*len + 1]))
			return (-1);
		b = (hex_value(line[*len]) << 4) | hex_value(line[*len + 1]);
		*num = (*num << 8) | b;
		*len += 2;
		if (checksum != NULL)
			*checksum = (*checksum + b) & 0xFF;
	}

	return (0);
}

static void
write_num(char *line, int *len, uint64_t num, size_t sz, int *checksum)
{
	uint8_t b;

	for (; sz > 0; sz--) {
		b = (num >> ((sz - 1) * 8)) & 0xFF;
		line[*len] = hex_digit((b >> 4) & 0xF);
		line[*len + 1] = hex_digit(b & 0xF);
		*len += 2;
		if (checksum != NULL)
			*checksum = (*checksum + b) & 0xFF;
	}
}

static char
hex_digit(uint8_t n)
{

	return ((n < 10) ? '0' + n : 'A' + (n - 10));
}

static int
hex_value(char x)
{

	if (isdigit(x))
		return (x - '0');
	else if (x >= 'a' && x <= 'f')
		return (x - 'a' + 10);
	else
		return (x - 'A' + 10);
}

static int
ishexdigit(char x)
{

	if (isdigit(x))
		return (1);
	if ((x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F'))
		return (1);

	return (0);
}
