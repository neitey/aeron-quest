#include "internal.h"

#include <stdarg.h>

static SDL_LogPriority Aeron_LogSdlPriority(AeronLogLevel level) {
	switch (level) {
		case AERON_LOG_TRACE:
			return SDL_LOG_PRIORITY_TRACE;
		case AERON_LOG_VERBOSE:
			return SDL_LOG_PRIORITY_VERBOSE;
		case AERON_LOG_DEBUG:
			return SDL_LOG_PRIORITY_DEBUG;
		case AERON_LOG_INFO:
			return SDL_LOG_PRIORITY_INFO;
		case AERON_LOG_WARN:
			return SDL_LOG_PRIORITY_WARN;
		case AERON_LOG_ERROR:
			return SDL_LOG_PRIORITY_ERROR;
		case AERON_LOG_CRITICAL:
			return SDL_LOG_PRIORITY_CRITICAL;
		default:
			return SDL_LOG_PRIORITY_INFO;
	}
}

/* Optional plain-text log file (AERON_LOG_FILE=path). GUI-subsystem Windows
 * builds have no console, so this is how a tester can send logs back. */
static SDL_IOStream* Aeron_LogFile(void) {
	static int           opened;
	static SDL_IOStream* file;
	if (!opened) {
		const char* path = SDL_getenv("AERON_LOG_FILE");
		opened           = 1;
		if (path && path[0]) {
			file = SDL_IOFromFile(path, "w");
		}
	}
	return file;
}

static const char* Aeron_LogLevelName(AeronLogLevel level) {
	switch (level) {
		case AERON_LOG_WARN:
			return "WARN ";
		case AERON_LOG_ERROR:
			return "ERROR";
		case AERON_LOG_CRITICAL:
			return "CRIT ";
		default:
			return "INFO ";
	}
}

void Aeron_LogMessageV(AeronLogLevel level, const char* category, const char* fmt, va_list args) {
	char            message[1024];
	SDL_LogPriority priority;
	SDL_IOStream*   file;

	priority = Aeron_LogSdlPriority(level);
	SDL_vsnprintf(message, sizeof(message), fmt ? fmt : "", args);
	SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s: %s", category ? category : "aeron", message);
	file = Aeron_LogFile();
	if (file) {
		SDL_IOprintf(file, "%10.3f %s %s: %s\n", (double)SDL_GetTicks() / 1000.0, Aeron_LogLevelName(level),
					 category ? category : "aeron", message);
		SDL_FlushIO(file);
	}
}

void Aeron_LogMessage(AeronLogLevel level, const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(level, category, fmt, args);
	va_end(args);
}

#if !defined(NDEBUG)
void Aeron_LogTrace(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_TRACE, category, fmt, args);
	va_end(args);
}
#endif

void Aeron_LogVerbose(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_VERBOSE, category, fmt, args);
	va_end(args);
}

#if !defined(NDEBUG)
void Aeron_LogDebug(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_DEBUG, category, fmt, args);
	va_end(args);
}
#endif

void Aeron_LogInfo(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_INFO, category, fmt, args);
	va_end(args);
}

void Aeron_LogWarn(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_WARN, category, fmt, args);
	va_end(args);
}

void Aeron_LogError(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_ERROR, category, fmt, args);
	va_end(args);
}

void Aeron_LogCritical(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_CRITICAL, category, fmt, args);
	va_end(args);
}

static AeronFatalErrorHandler g_fatal_error_handler;
static int                    g_fatal_error_handler_active;

void Aeron_SetFatalErrorHandler(AeronFatalErrorHandler handler) { g_fatal_error_handler = handler; }

void Aeron_FatalError(const char* title, const char* message) {
	if (g_fatal_error_handler && !g_fatal_error_handler_active) {
		g_fatal_error_handler_active = 1;
		g_fatal_error_handler(title ? title : "Aeron", message ? message : "Fatal error");
		g_fatal_error_handler_active = 0;
		return;
	}
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title ? title : "Aeron", message ? message : "Fatal error",
							 g_aeron.window);
}

void Aeron_RequestFatalError(const char* title, const char* message) {
	if (g_aeron.fatal_error_requested) {
		return;
	}
	g_aeron.fatal_error_requested = 1;
	g_aeron.quit_requested        = 1;
	Aeron_LogCritical("aeron", "fatal: %s", message ? message : "Fatal error");
	Aeron_FatalError(title, message);
}
