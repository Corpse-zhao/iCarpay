/*
 * btstack_legacy.c — 旧版 BTstack run-loop API 兼容垫片
 *
 * 上游 Showcase 的 carplay_bt（我们的 CarDisplaySim）是按旧版 BTstack 的
 * run_loop_* 命名写的；当前 btstack-rootless 树里只有 btstack_run_loop_*。
 * 这里做一层薄映射编进 libBTstack.dylib，不改上游代码、不改调用方。
 *
 * 注意：两边回调都只收到同一个 timer 指针，签名直接转调即可。
 * 调用方用 128 字节清零缓冲区当 opaque btstack_timer_source_t，
 * 链表项为零即符合 btstack_linked_item_t 的初始要求。
 */

#include <stdint.h>
#include "btstack_run_loop.h"
#include "btstack_run_loop_posix.h"

void run_loop_init(int type) {
    /* 旧 API 的 RUN_LOOP_POSIX == 1；无论传什么都用 posix 实现 */
    (void)type;
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
}

void run_loop_execute(void) {
    btstack_run_loop_execute();
}

void run_loop_set_timer(void *timer, uint32_t milliseconds) {
    btstack_run_loop_set_timer((btstack_timer_source_t *)timer, milliseconds);
}

void run_loop_set_timer_handler(void *timer, void (*handler)(void *timer)) {
    btstack_run_loop_set_timer_handler((btstack_timer_source_t *)timer,
        (void (*)(btstack_timer_source_t *))handler);
}

void run_loop_add_timer(void *timer) {
    btstack_run_loop_add_timer((btstack_timer_source_t *)timer);
}

int run_loop_remove_timer(void *timer) {
    return btstack_run_loop_remove_timer((btstack_timer_source_t *)timer);
}
