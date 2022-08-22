/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2022 1BitSquared <info@1bitsquared.com>
 * Written by Rafael Silva <perigoso@riseup.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* This file implements target flash interaction routines.
 * Provides functionality for buffered flash operations
 * It depends on target flash implementations
 */

#include "general.h"
#include "target_internal.h"

static int target_flash_write_buffered(target_flash_s *f, target_addr_t dest, const void *src, size_t len);
static int target_flash_done_buffered(target_flash_s *f);

target_flash_s *target_flash_for_addr(target *t, uint32_t addr)
{
	for (target_flash_s *f = t->flash; f; f = f->next)
		if ((f->start <= addr) && (addr < (f->start + f->length)))
			return f;
	return NULL;
}

static int target_enter_flash_mode(target *t)
{
	if (t->flash_mode)
		return 0;

	int ret = 0;
	if (t->enter_flash_mode)
		ret = t->enter_flash_mode(t);
	else
		/* Reset target on flash command */
		/* This saves us if we're interrupted in IRQ context */
		target_reset(t);

	if (ret == 0)
		t->flash_mode = true;

	return ret;
}

static int target_exit_flash_mode(target *t)
{
	if (!t->flash_mode)
		return 0;

	int ret = 0;
	if (t->exit_flash_mode)
		ret = t->exit_flash_mode(t);
	else
		/* Reset target to known state when done flashing */
		target_reset(t);

	t->flash_mode = false;

	return ret;
}

static int flash_prepare(target_flash_s *f)
{
	if (f->ready)
		return 0;

	int ret = 0;
	if (f->prepare)
		ret = f->prepare(f);

	if (ret == 0)
		f->ready = true;

	return ret;
}

static int flash_done(target_flash_s *f)
{
	if (!f->ready)
		return 0;

	int ret = 0;
	if (f->done)
		ret = f->done(f);

	if (f->buf) {
		free(f->buf);
		f->buf = NULL;
	}

	f->ready = false;

	return ret;
}

int target_flash_erase(target *t, target_addr_t addr, size_t len)
{
	if (target_enter_flash_mode(t) != 0)
		return 1;

	int ret = 0;
	while (len) {
		target_flash_s *f = target_flash_for_addr(t, addr);
		if (!f) {
			DEBUG_WARN("Requested address is outside the valid range 0x%06" PRIx32 "\n", addr);
			return 1;
		}

		const target_addr_t local_start_addr = addr & ~(f->blocksize - 1U);
		const target_addr_t local_end_addr = local_start_addr + f->blocksize;

		if (flash_prepare(f) == 0)
			ret |= f->erase(f, local_start_addr, f->blocksize);
		flash_done(f);

		len -= MIN(local_end_addr - addr, len);
		addr = local_end_addr;
	}
	return ret;
}

int target_flash_write(target *t, target_addr_t dest, const void *src, size_t len)
{
	if (target_enter_flash_mode(t) != 0)
		return 1;

	int ret = 0;
	while (len) {
		target_flash_s *f = target_flash_for_addr(t, dest);
		if (!f)
			return 1;

		flash_prepare(f)

		size_t tmptarget = MIN(dest + len, f->start + f->length);
		size_t tmplen = tmptarget - dest;
		ret |= target_flash_write_buffered(f, dest, src, tmplen);
		dest += tmplen;
		src += tmplen;
		len -= tmplen;
		/* If the current chunk of Flash is now full from this operation
		 * then finish operations on the Flash chunk and free the internal buffer.
		 */
		if (dest == f->start + f->length)
			ret |= target_flash_done_buffered(f);
	}
	return ret;
}

int target_flash_complete(target *t)
{
	if (!t->flash_mode)
		return -1;

	int ret = 0;
	for (target_flash_s *f = t->flash; f; f = f->next) {
		ret |= target_flash_done_buffered(f);
		ret |= flash_done(f);
	}
	target_exit_flash_mode(t);
	return ret;
}

int target_flash_write_buffered(target_flash_s *f, target_addr_t dest, const void *src, size_t len)
{
	int ret = 0;

	if (f->buf == NULL) {
		/* Allocate flash sector buffer */
		f->buf = malloc(f->writesize);
		if (!f->buf) { /* malloc failed: heap exhaustion */
			DEBUG_WARN("malloc: failed in %s\n", __func__);
			return 1;
		}
		f->buf_addr = -1;
	}
	while (len) {
		uint32_t offset = dest % f->writesize;
		uint32_t base = dest - offset;
		if (base != f->buf_addr) {
			if (f->buf_addr != (uint32_t)-1) {
				/* Write sector to flash if valid */
				ret |= f->write(f, f->buf_addr, f->buf, f->writesize);
			}
			/* Setup buffer for a new sector */
			f->buf_addr = base;
			memset(f->buf, f->erased, f->writesize);
		}
		/* Copy chunk into sector buffer */
		size_t sectlen = MIN(f->writesize - offset, len);
		memcpy(f->buf + offset, src, sectlen);
		dest += sectlen;
		src += sectlen;
		len -= sectlen;
	}
	return ret;
}

int target_flash_done_buffered(target_flash_s *f)
{
	int ret = 0;
	if ((f->buf != NULL) && (f->buf_addr != (uint32_t)-1)) {
		/* Write sector to flash if valid */
		ret = f->write(f, f->buf_addr, f->buf, f->writesize);
		f->buf_addr = -1;
		free(f->buf);
		f->buf = NULL;
	}

	return ret;
}