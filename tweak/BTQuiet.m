/* BTQuiet: keeps Bluetooth clients from flooding launchd while Showcase
 * owns the Bluetooth controller.
 *
 * Ships only in the Taurine package (com.rostane.showcase-taurine).
 *
 * Showcase unloads bluetoothd so BTstack can open the UART. Every client of
 * bluetoothd (mediaserverd's audio plugin, findmydeviced, sharingd,
 * locationd, routined, contextstored, ...) then reconnects in a tight loop:
 * hundreds of attempts a second on iOS 14. Each attempt goes through launchd
 * and, on Taurine, jailbreakd; the load average climbs past 100 and BTstack
 * answers the iPhone seconds late, so iAP2 times out ("Device not
 * supported").
 *
 * Only while Showcase holds the controller (SHOWCASE_BT_OWNED exists; /tmp
 * is emptied at boot) two reconnect paths are spaced a second apart:
 *   - BTSessionAttachWithQueue, the MobileBluetooth attach every client
 *     retries;
 *   - mediaserverd's connection to com.apple.BTAudioHALPlugin.xpc.
 * The caller sleeps; nothing is deferred to another queue, so no lock or
 * dispatch_once can be waited on from two sides. The main thread of a UIKit
 * process never sleeps. */
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
/* Only the connection type is needed; some iOS SDKs ship no xpc headers. */
typedef void *xpc_connection_t;

#define SHOWCASE_BT_OWNED "/tmp/showcase_bt_owned"
#define MOBILE_BLUETOOTH "/System/Library/PrivateFrameworks/MobileBluetooth.framework/MobileBluetooth"
#define AUDIO_PLUGIN_SERVICE "com.apple.BTAudioHALPlugin.xpc"
#define MIN_INTERVAL_NS (1000ull * 1000 * 1000)

extern const char *getprogname(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t attach_next_ns, audio_next_ns;
static bool ui_process;

static uint64_t now_ns(void) {
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return mach_absolute_time() * base.numer / base.denom;
}

/* Sleeps until a second after the previous call through `next`. */
static void space_out(uint64_t *next) {
    if (access(SHOWCASE_BT_OWNED, F_OK) != 0) return;
    if (ui_process && pthread_main_np()) return;
    pthread_mutex_lock(&lock);
    uint64_t now = now_ns(), at = *next > now ? *next : now;
    *next = at + MIN_INTERVAL_NS;
    pthread_mutex_unlock(&lock);
    if (at > now) usleep((useconds_t)((at - now) / 1000));
}

static int (*attach_original)(const char *, const void *, void *, dispatch_queue_t);

static int attach(const char *name, const void *callbacks, void *context, dispatch_queue_t queue) {
    space_out(&attach_next_ns);
    return attach_original(name, callbacks, context, queue);
}

static xpc_connection_t (*create_original)(const char *, dispatch_queue_t, uint64_t);

static xpc_connection_t create(const char *name, dispatch_queue_t queue, uint64_t flags) {
    if (name && !strcmp(name, AUDIO_PLUGIN_SERVICE)) space_out(&audio_next_ns);
    return create_original(name, queue, flags);
}

static void *hook_symbol(const char *name) {
    static const char *libraries[] = {
        "/usr/lib/libhooker.dylib", "/var/jb/usr/lib/libhooker.dylib",
        "/usr/lib/libsubstrate.dylib", "/var/jb/usr/lib/libsubstrate.dylib",
        "/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate",
        "/usr/lib/libellekit.dylib", "/var/jb/usr/lib/libellekit.dylib", NULL};
    void *symbol = dlsym(RTLD_DEFAULT, name);
    for (const char **path = libraries; !symbol && *path; path++) {
        if (access(*path, R_OK)) continue;
        void *library = dlopen(*path, RTLD_NOW);
        if (library) symbol = dlsym(library, name);
    }
    return symbol;
}

static void hook(void *target, void *replacement, void **saved) {
    if (!target) return;
    void (*ms_function)(void *, void *, void **) = hook_symbol("MSHookFunction");
    if (ms_function) ms_function(target, replacement, saved);
    if (*saved) return;
    struct { void *function, *replacement, *original, *options; } request = {target, replacement, saved, NULL};
    int (*lh_functions)(void *, int) = hook_symbol("LHHookFunctions");
    if (lh_functions) lh_functions(&request, 1);
}

static void hook_attach(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *library = dlopen(MOBILE_BLUETOOTH, RTLD_NOLOAD | RTLD_NOW);
        if (library) hook(dlsym(library, "BTSessionAttachWithQueue"), (void *)attach, (void **)&attach_original);
    });
}

/* Some daemons load MobileBluetooth after launch. */
static void image_added(const struct mach_header *header, intptr_t slide) {
    Dl_info info;
    if (attach_original || !dladdr(header, &info) || !info.dli_fname || strcmp(info.dli_fname, MOBILE_BLUETOOTH)) return;
    hook_attach();
}

/* Loaded into every Foundation process; only Bluetooth clients are hooked,
 * and the hooks do nothing outside a Showcase session. */
__attribute__((constructor)) static void init(void) {
    const char *name = getprogname();
    if (!strcmp(name, "SpringBoard") || !strcmp(name, "backboardd")) return;
    ui_process = dlsym(RTLD_DEFAULT, "UIApplicationMain") != NULL;
    if (!strcmp(name, "mediaserverd"))
        hook(dlsym(RTLD_DEFAULT, "xpc_connection_create_mach_service"), (void *)create, (void **)&create_original);
    if (dlopen(MOBILE_BLUETOOTH, RTLD_NOLOAD | RTLD_NOW)) hook_attach();
    else _dyld_register_func_for_add_image(image_added);
}
