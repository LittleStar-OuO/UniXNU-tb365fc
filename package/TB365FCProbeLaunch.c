// SPDX-License-Identifier: MIT
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

int tb365fc_mali_probe_spawn(void *process, uint64_t scratch, uint64_t init_envp);

extern void IOSleep(unsigned int milliseconds);

static vfs_context_t tb365fc_log_ctx;
static char tb365fc_log_path[] = "/private/var/ngpu/mali-vk-probe.log";

/* The probe's own stdout goes to /dev/console through the small text console,
 * which the large-font console patch does not render. Echo its log file through
 * kprintf instead: the console keeps a 4 KiB window and the file is tiny. */
static void
tb365fc_probe_log_thread(void *arg __unused, wait_result_t wres __unused)
{
	uint8_t *buf = kalloc_data(4096, Z_WAITOK);
	uint64_t last = 0;
	int dumps = 0;
	if (!buf) {
		return;
	}
	kprintf("tb365fc: probe log watcher started\n");
	while (dumps < 4 && last < 0xffffffffULL) {
		IOSleep(30000);
		vnode_t vp = NULLVP;
		if (vnode_lookup(tb365fc_log_path, 0, &vp, tb365fc_log_ctx) != 0) {
			continue;
		}
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_WANTED(&va, va_data_size);
		if (vnode_getattr(vp, &va, tb365fc_log_ctx) == 0 &&
		    va.va_data_size && va.va_data_size != last) {
			size_t len = va.va_data_size;
			int resid = 0;
			if (len > 4000) {
				len = 4000;
			}
			if (vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)len, 0,
			    UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(tb365fc_log_ctx),
			    &resid, vfs_context_proc(tb365fc_log_ctx)) == 0) {
				last = va.va_data_size;
				dumps++;
				kprintf("plog| begin bytes=%llu\n", last);
				uint8_t *p = buf;
				int left = (int)(len - resid);
				int lines = 0;
				while (left > 0 && lines < 64) {
					uint8_t *nl = memchr(p, '\n', left);
					int n = nl ? (int)(nl - p) : left;
					char line[200];
					if (n > 195) {
						n = 195;
					}
					memcpy(line, p, n);
					line[n] = 0;
					if (n) {
						kprintf("plog| %s\n", line);
						lines++;
					}
					if (!nl) {
						break;
					}
					left -= n + 1;
					p = nl + 1;
				}
				kprintf("plog| end\n");
			}
		}
		vnode_put(vp);
		if (last == 0xffffffffULL) {
			break;
		}
	}
	kfree_data(buf, 4096);
}

int
tb365fc_mali_probe_spawn(void *process, uint64_t scratch, uint64_t init_envp)
{
	static int attempted;
	uint32_t enabled = 0;
	if (attempted || !PE_parse_boot_argn("native-mali-probe-spawn", &enabled, sizeof(enabled)) || enabled != 1) {
		return 0;
	}
	attempted = 1;

	/* Mounts use 0x100..0x9ff and mkdir uses 0xc00..0xcbf. The owner
	 * cache environment survives in 0xd00..0xfff until both execs consume it. */
	static const char path[] = "/private/var/ngpu/bin/mali-vk-probe";
	const user_addr_t path_addr = scratch + 0xa00;
	const user_addr_t argv_addr = scratch + 0xa80;
	const user_addr_t empty_env_addr = scratch + 0xa90;
	const user_addr_t pid_addr = scratch + 0xaa0;
	const user64_addr_t argv[2] = {path_addr, USER_ADDR_NULL};
	const user64_addr_t empty_env[1] = {USER_ADDR_NULL};
	int32_t pid = 0;
	int error = copyout(path, path_addr, sizeof(path));
	if (!error) error = copyout(argv, argv_addr, sizeof(argv));
	if (!error && !init_envp) error = copyout(empty_env, empty_env_addr, sizeof(empty_env));
	if (!error) error = copyout(&pid, pid_addr, sizeof(pid));
	if (!error) {
		struct posix_spawn_args request = {
			.pid = pid_addr, .path = path_addr, .adesc = USER_ADDR_NULL,
			.argv = argv_addr, .envp = init_envp ? init_envp : empty_env_addr,
		};
		int32_t result = 0;
		kprintf("tb365fc: mali-vk-probe spawning env=%s\n", init_envp ? "owner-cache" : "default");
		error = posix_spawn((proc_t)process, &request, &result);
		if (!error) error = copyin(pid_addr, &pid, sizeof(pid));
	}
	kprintf("tb365fc: mali-vk-probe spawn %d (pid %d)\n", error, pid);
	if (!error) {
		tb365fc_log_ctx = vfs_context_create(vfs_context_current());
		if (tb365fc_log_ctx) {
			thread_t watcher = NULL;
			if (kernel_thread_start(tb365fc_probe_log_thread, NULL, &watcher) == KERN_SUCCESS) {
				thread_deallocate(watcher);
			}
		}
	}
	return error;
}
