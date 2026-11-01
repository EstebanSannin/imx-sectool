// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- SCU reply and timeout tests without hardware access.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#include "../src/scu_uio.c"
#include <assert.h>

const struct soc_desc *soc;

int main(void)
{
	uint32_t regs[16] = { 0 }, reply[8];
	struct scu_mu mu = { .fd = -1, .lock = -1, .regs = regs };
	char path[] = "/tmp/imx-sectool-test-XXXXXX";
	struct stat st;

	assert(reply_status(&mu, 0x00010401, 17) == 0);
	assert(reply_status(&mu, 0x00010201, 19) == 0);
	assert(reply_status(&mu, 0x03010201, 19) == 3);
	assert(reply_status(&mu, 0x07010201, 19) == 7);
	assert(reply_status(&mu, 0x03010101, 19) == -1);
	assert(reply_status(&mu, 0x00010202, 19) == -1);
	assert(reply_status(&mu, 0x00090201, 19) == -1);
	assert(reply_status(&mu, 0x00010001, 19) == -1);
	assert(reply_status(&mu, 0x00010901, 19) == -1);
	assert(reply_status(&mu, 0x00010201, 17) == -1);
	assert(idle(&mu) == -1);
	regs[MU_SR / 4] = 0x00f00200;
	assert(idle(&mu) == 0);
	regs[MU_CR / 4] = 1;
	assert(idle(&mu) == -1);
	regs[MU_CR / 4] = 0;
	regs[MU_SR / 4] |= 1u << 27;
	assert(idle(&mu) == -1);
	regs[MU_SR / 4] = 0x00f00200;
	assert(wait_flag(&mu, 1u << 27, now_ms()) == -1);
	assert(scu_call(&mu, 18, 0, reply) == -1);
	assert(scu_call(&mu, 19, 256, reply) == -1);
	mu.lock = mkstemp(path);
	assert(mu.lock >= 0);
	unlink(path);
	assert(scu_call(&mu, 19, 0, reply) == -1);
	assert(strstr(mu.error, "timed out"));
	assert(!fstat(mu.lock, &st) && st.st_size == 11);
	close(mu.lock);
	puts("SCU protocol and timeout tests passed");
	return 0;
}
