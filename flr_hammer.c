/* Hammer SS0/SS1/FEAT_OVR_PLM from the host across one PCI FLR.
 * The post-reset mask on GB202 is 0x80842bcf (PL2+PL3) for ~1.3s. This card
 * has never been written during that window. Bounded; does not touch flash.
 *
 *   flr_hammer /sys/bus/pci/devices/0000:08:00.0/resource0 \
 *              /sys/bus/pci/devices/0000:08:00.0/reset
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define MAP_LEN 0x00900000u
#define BOOT    0x00000000u
#define FUSE_PLM 0x008200fcu
#define FEAT_PLM 0x00823804u
#define SS0     0x0082381cu
#define SS1     0x00823820u
#define READOUT 0x00823818u

#define SS0_WANT 0x88888888u
#define SS1_WANT 0x00000008u
#define PLM_OPEN 0xfffffff0u /* write field 0xf: all levels */
#define WINDOW_MS 2500

static volatile uint32_t *bar;

static uint32_t rd(uint32_t off)
{
	return bar[off / 4];
}

static void wr(uint32_t off, uint32_t v)
{
	bar[off / 4] = v;
}

static double ms_since(const struct timespec *t0)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t0->tv_sec) * 1000.0 +
	       (now.tv_nsec - t0->tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
	const char *res = argc > 1 ? argv[1]
				   : "/sys/bus/pci/devices/0000:08:00.0/resource0";
	const char *rst = argc > 2 ? argv[2]
				   : "/sys/bus/pci/devices/0000:08:00.0/reset";
	int fd, rfd, samples = 0, stuck = 0, plm_stuck = 0, transitions = 0;
	uint32_t boot, plm, ss0, ss1, fuse_plm, last_plm = 0, last_ss0 = 0;
	uint32_t first_plm = 0;
	struct timespec t0;
	int saw_live = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open(res, O_RDWR | O_SYNC);
	if (fd < 0) {
		perror(res);
		return 1;
	}
	bar = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (bar == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	boot = rd(BOOT);
	plm = rd(FEAT_PLM);
	ss0 = rd(SS0);
	ss1 = rd(SS1);
	fuse_plm = rd(FUSE_PLM);
	printf("before boot=%#x plm=%#x ss0=%#x ss1=%#x fuse_plm=%#x readout=%#x\n",
	       boot, plm, ss0, ss1, fuse_plm, rd(READOUT));
	if (boot != 0x1b6000a1u) {
		printf("abort: PMC_BOOT_0 is not GB206 %#x\n", boot);
		return 2;
	}

	rfd = open(rst, O_WRONLY);
	if (rfd < 0) {
		perror(rst);
		return 1;
	}
	if (write(rfd, "1", 1) != 1) {
		perror("reset");
		return 1;
	}
	close(rfd);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	printf("flr returned\n");

	for (;;) {
		double ms = ms_since(&t0);
		uint32_t b, p, s0, s1, p2, s0b, s1b;

		if (ms > WINDOW_MS)
			break;
		b = rd(BOOT);
		if (b == 0xffffffffu) {
			if (ms > 300 && !saw_live) {
				printf("dead at %.1f ms, stop\n", ms);
				break;
			}
			continue;
		}
		saw_live = 1;
		p = rd(FEAT_PLM);
		s0 = rd(SS0);
		s1 = rd(SS1);
		wr(FEAT_PLM, PLM_OPEN);
		wr(SS0, SS0_WANT);
		wr(SS1, SS1_WANT);
		p2 = rd(FEAT_PLM);
		s0b = rd(SS0);
		s1b = rd(SS1);
		samples++;
		if (samples == 1) {
			first_plm = p;
			printf("first %.1f ms boot=%#x plm=%#x write_mask=%#x ss0=%#x ss1=%#x fuse_plm=%#x\n",
			       ms, b, p, (p >> 4) & 0xfu, s0, s1, rd(FUSE_PLM));
		}
		if (p != last_plm || s0 != last_ss0) {
			printf("chg %.1f ms plm %#x -> %#x (mask %#x) ss0 %#x -> %#x after plm=%#x ss0=%#x ss1=%#x\n",
			       ms, last_plm, p, (p >> 4) & 0xfu, last_ss0, s0, p2, s0b, s1b);
			last_plm = p;
			last_ss0 = s0;
			transitions++;
			if (transitions > 40) {
				printf("too many transitions, stop\n");
				break;
			}
		}
		if (s0b == SS0_WANT || s1b == SS1_WANT) {
			stuck++;
			printf("STUCK %.1f ms plm=%#x ss0=%#x ss1=%#x\n", ms, p2, s0b, s1b);
		}
		if (p2 == PLM_OPEN)
			plm_stuck++;
	}

	printf("final boot=%#x plm=%#x ss0=%#x ss1=%#x fuse_plm=%#x readout=%#x\n",
	       rd(BOOT), rd(FEAT_PLM), rd(SS0), rd(SS1), rd(FUSE_PLM), rd(READOUT));
	printf("verdict samples=%d stuck=%d plm_open_writes=%d first_plm=%#x\n",
	       samples, stuck, plm_stuck, first_plm);
	return stuck ? 0 : 3;
}
