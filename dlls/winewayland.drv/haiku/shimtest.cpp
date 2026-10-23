/* Exercises hglshim.cpp from C: contexts on separate threads, readback orientation, many contexts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <OS.h>
#include <GL/gl.h>
extern "C" {
struct hgl_context;
int hgl_init(void);
hgl_context *hgl_context_create(int, int, int, int, int, int);
void hgl_context_destroy(hgl_context *);
int hgl_make_current(hgl_context *);
void hgl_release_current(hgl_context *);
int hgl_read_pixels(hgl_context *, int, int, void *, int);
void *hgl_proc_address(const char *);
}
static int32 worker(void *p)
{
	hgl_context *c = (hgl_context *)p;
	hgl_make_current(c);
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	unsigned char px[16 * 16 * 4];
	hgl_read_pixels(c, 16, 16, px, 64);
	printf("worker thread read: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
	hgl_release_current(c);
	return 0;
}
int main()
{
	printf("init: %d\n", hgl_init());
	bigtime_t t0 = system_time();
	hgl_context *a = hgl_context_create(1920, 1080, 1, 1, 1, 1);
	printf("context %p created in %lld ms\n", a, (long long)(system_time() - t0) / 1000);
	hgl_make_current(a);
	glViewport(0, 0, 64, 32);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, 64, 8);			/* the bottom 8 rows (GL origin is bottom left) */
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	static unsigned char out[64 * 32 * 4];
	printf("read: %d\n", hgl_read_pixels(a, 64, 32, out, 64 * 4));
	printf("top row first pixel (expect black, B G R A = 0 0 0 255): %u %u %u %u\n", out[0], out[1], out[2], out[3]);
	unsigned char* bottom = out + 31 * 64 * 4;
	printf("bottom row first pixel (expect red, B G R A = 0 0 255 255): %u %u %u %u\n", bottom[0], bottom[1], bottom[2], bottom[3]);
	printf("glGenFramebuffers via glapi: %p\n", hgl_proc_address("glGenFramebuffers"));
	printf("GL_VERSION %s\n", glGetString(GL_VERSION));
	hgl_release_current(a);

	hgl_context *b = hgl_context_create(640, 480, 1, 1, 1, 1);
	thread_id t = spawn_thread(worker, "w", B_NORMAL_PRIORITY, b);
	resume_thread(t);
	status_t rc; wait_for_thread(t, &rc);
	hgl_make_current(a);
	hgl_release_current(a);
	t0 = system_time();
	hgl_context *many[6];
	for (int i = 0; i < 6; i++) many[i] = hgl_context_create(800, 600, 1, 1, 1, 1);
	printf("6 more contexts in %lld ms\n", (long long)(system_time() - t0) / 1000);
	for (int i = 0; i < 6; i++) hgl_context_destroy(many[i]);
	hgl_context_destroy(b);
	hgl_context_destroy(a);
	printf("done\n");
	return 0;
}
