/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "application.h"
#include "scenes/lobby.h"
#include "scenes/stream.h"
#include "spdlog/spdlog.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __ANDROID__
#include "spdlog/sinks/android_sink.h"
#include <android/native_window.h>
#include <android_native_app_glue.h>
#else
#include "spdlog/sinks/stdout_color_sinks.h"
#endif

#ifdef __ANDROID__
void real_main(android_app * native_app)
#else
void real_main()
#endif
{
	try
	{
		application_info info;
#ifdef __ANDROID__
		info.native_app = native_app;
#endif
		info.name = "WiVRn";
		info.version = VK_MAKE_VERSION(1, 0, 0);
		application app(info);

		std::string server_address = app.get_server_address();
		if (not server_address.empty())
		{
			auto colon = server_address.rfind(":");
			int port = xrt::drivers::wivrn::default_port;
			if (colon != std::string::npos)
			{
				port = std::stoi(server_address.substr(colon + 1));
				server_address = server_address.substr(0, colon);
			}
			auto & config = application::get_config();
			config.servers["wivrn://"] = configuration::server_data{
			        .autoconnect = true,
			        .manual = true,
			        .visible = true,
			        .compatible = true,
			        .service = {
			                .name = app.get_server_address(),
			                .hostname = server_address,
			                .port = port,
			                .tcp_only = app.get_server_tcp_only(),
			        },
			};
		}
		app.push_scene<scenes::lobby>();

		app.run();
	}
	catch (std::runtime_error & e)
	{
		spdlog::error("Caught runtime error: \"{}\"", e.what());
	}
	catch (std::exception & e)
	{
		spdlog::error("Caught exception: \"{}\"", e.what());
	}
	catch (...)
	{
		spdlog::error("Caught unknown exception");
	}

#ifdef __ANDROID__
	ANativeActivity_finish(native_app->activity);

	// Read all pending events.
	while (!native_app->destroyRequested)
	{
		int events;
		struct android_poll_source * source;

		while (ALooper_pollAll(100, nullptr, &events, (void **)&source) >= 0)
		{
			// Process this event.
			if (source != nullptr)
				source->process(native_app, source);
		}
	}
	exit(0);
#endif
}

#ifdef __ANDROID__
void android_main(android_app * native_app) __attribute__((visibility("default")));
void android_main(android_app * native_app)
{
	static auto logger = spdlog::android_logger_mt("WiVRn", "WiVRn");

	spdlog::set_default_logger(logger);

	real_main(native_app);
}
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/NSObjCRuntime.h>
#include <CoreFoundation/CFBundle.h>
#include "arkit_setup.h"
extern "C" [[clang::not_tail_called]] void NSLog(id format, ...);
extern "C" id NSStringFromClass(Class);
extern "C" int UIApplicationMain(int, char*[], id, id);
void real_main();

static uint32_t WriteBlock(char *const head, const char *const head_end, const char *const data, const char *const data_end) {
	size_t length = (size_t)(head_end - head);
	if(length > (size_t)(data_end - data))
		length = (size_t)(data_end - data);
	memcpy(head, data, length);
	return (uint32_t)length;
}

static id logFormat;
static void DoLog(const char prefix[], char line[4096], uint32_t *const line_len, const char buffer[], const uint32_t size) {
	const char *buffer_it = buffer;
	for(const char *end; (end = (const char*)memchr(buffer_it, '\n', (size_t)(&buffer[size] - buffer_it))) != NULL; *line_len = 0, buffer_it = &end[1])
		NSLog(logFormat, prefix, *line_len + WriteBlock(&line[*line_len], &line[4096], buffer_it, end), line);
	*line_len += WriteBlock(&line[*line_len], &line[4096], buffer_it, &buffer[size]);
}

extern "C" const CFStringRef UIApplicationLaunchOptionsShortcutItemKey;
int main(const int argc, char *argv[]) {
	static std::thread appThread = {};

	logFormat = ((id (*)(Class, SEL, const char[]))objc_msgSend)(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "%s%.*s");
	stdout->_write = [](void *const _unused, const char *const buffer, const int size) {
		thread_local static uint32_t line_len = 0;
		thread_local static char line[4096] = {0};
		DoLog("{stdout}: ", line, &line_len, buffer, (uint32_t)size);
		return size;
	};
	stderr->_write = [](void *const _unused, const char *const buffer, const int size) {
		thread_local static uint32_t line_len = 0;
		thread_local static char line[4096] = {0};
		DoLog("{stderr}: ", line, &line_len, buffer, (uint32_t)size);
		return size;
	};

	CFURLRef url = CFBundleCopyResourcesDirectoryURL(CFBundleGetMainBundle());
	char path[PATH_MAX + 20];
	if(!CFURLGetFileSystemRepresentation(url, true, (uint8_t*)path, sizeof(path) - 20)) {
		fprintf(stderr, "CFURLGetFileSystemRepresentation(mainBundle) failed\n");
		return -1;
	}
	char *const path_end = &path[strlen(path)];
	CFRelease(url);

	memcpy(path_end, "/openxr_monado.json", 20);
	setenv("XR_RUNTIME_JSON", path, false);
	memcpy(path_end, "/assets", 8);
	setenv("WIVRN_ASSET_ROOT", path, false);
	memcpy(path_end, "/locale", 8);
	setenv("WIVRN_LOCALE_ROOT", path, false);

	const id defaultManager = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSFileManager"), sel_registerName("defaultManager"));
	const CFArrayRef urls = ((CFArrayRef (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(defaultManager,
		sel_registerName("URLsForDirectory:inDomains:"), 9/*NSDocumentDirectory*/, 1/*NSUserDomainMask*/);
	if(!CFURLGetFileSystemRepresentation((CFURLRef)CFArrayGetValueAtIndex(urls, CFArrayGetCount(urls) - 1), true, (uint8_t*)path, sizeof(path) - 20)) {
		fprintf(stderr, "CFURLGetFileSystemRepresentation(documents) failed\n");
		return -1;
	}

	setenv("XDG_CONFIG_HOME", path, false);
	setenv("XDG_CACHE_HOME", path, false);

	const Class delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), "AppDelegate", 0);
	if(!class_addMethod(delegateClass, sel_registerName("application:didFinishLaunchingWithOptions:"), (IMP)+[](void *a, SEL b, void *c, CFDictionaryRef launchOptions) -> bool {
		constexpr auto start = []() {appThread = std::thread(real_main);};
		if(const void *value = nullptr; launchOptions != nullptr &&
				CFDictionaryGetValueIfPresent(launchOptions, UIApplicationLaunchOptionsShortcutItemKey, &value) && value != nullptr)
			StartARKitCalibration(start);
		else
			start();
		return true;
	}, "c@:@@")) {
		fprintf(stderr, "class_addMethod(\"application:didFinishLaunchingWithOptions:\") failed\n");
		return -1;
	}
	if(!class_addMethod(delegateClass, sel_registerName("application:applicationWillTerminate:"), (IMP)+[](void*, SEL, void*) {
		application::request_exit();
		fprintf(stderr, "AppDelegate exiting\n");
		appThread.join();
		appThread = {};
		fprintf(stderr, "AppDelegate exit SUCCESS\n");
	}, "v@:@")) {
		fprintf(stderr, "class_addMethod(\"application:applicationWillTerminate:\") failed\n");
		return -1;
	}
	objc_registerClassPair(delegateClass);
	return UIApplicationMain(argc, argv, nullptr, NSStringFromClass(delegateClass));
}
#else
int main(int argc, char * argv[])
{
	spdlog::set_default_logger(spdlog::stdout_color_mt("WiVRn"));

	char * loglevel = getenv("WIVRN_LOGLEVEL");
	if (loglevel)
	{
		if (!strcasecmp(loglevel, "trace"))
			spdlog::set_level(spdlog::level::trace);
		else if (!strcasecmp(loglevel, "debug"))
			spdlog::set_level(spdlog::level::debug);
		else if (!strcasecmp(loglevel, "info"))
			spdlog::set_level(spdlog::level::info);
		else if (!strcasecmp(loglevel, "warning"))
			spdlog::set_level(spdlog::level::warn);
		else if (!strcasecmp(loglevel, "error"))
			spdlog::set_level(spdlog::level::err);
		else if (!strcasecmp(loglevel, "critical"))
			spdlog::set_level(spdlog::level::critical);
		else if (!strcasecmp(loglevel, "off"))
			spdlog::set_level(spdlog::level::off);
		else
			spdlog::warn("Invalid value for WIVRN_LOGLEVEL environment variable");
	}

	real_main();
}
#endif
