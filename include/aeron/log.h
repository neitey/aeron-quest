#ifndef AERON_LOG_H
#define AERON_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AeronLogLevel {
	AERON_LOG_TRACE,
	AERON_LOG_VERBOSE,
	AERON_LOG_DEBUG,
	AERON_LOG_INFO,
	AERON_LOG_WARN,
	AERON_LOG_ERROR,
	AERON_LOG_CRITICAL,
} AeronLogLevel;

/* Dynamic-level entry points for log adapters and forwarding callbacks. */
void Aeron_LogMessageV(AeronLogLevel level, const char* category, const char* fmt, va_list args);
void Aeron_LogMessage(AeronLogLevel level, const char* category, const char* fmt, ...);

/* Trace and debug calls are removed from builds that define NDEBUG. */
#if defined(NDEBUG)
#define Aeron_LogTrace(...) ((void)0)
#else
void Aeron_LogTrace(const char* category, const char* fmt, ...);
#endif
void Aeron_LogVerbose(const char* category, const char* fmt, ...);
#if defined(NDEBUG)
#define Aeron_LogDebug(...) ((void)0)
#else
void Aeron_LogDebug(const char* category, const char* fmt, ...);
#endif
void Aeron_LogInfo(const char* category, const char* fmt, ...);
void Aeron_LogWarn(const char* category, const char* fmt, ...);
void Aeron_LogError(const char* category, const char* fmt, ...);
/* Critical records severity only; it does not request shutdown or show a dialog. */
void Aeron_LogCritical(const char* category, const char* fmt, ...);

/* Shows a modal fatal-error message box attached to the Aeron window when available. */
void Aeron_FatalError(const char* title, const char* message);

/* Replaces the fatal-error message box, e.g. with an in-game screen where OS
 * dialogs cannot be seen (immersive VR). NULL restores the message box. The
 * handler is not re-entered: a fatal error raised while it runs falls back to
 * the message box. */
typedef void (*AeronFatalErrorHandler)(const char* title, const char* message);
void Aeron_SetFatalErrorHandler(AeronFatalErrorHandler handler);

#ifdef __cplusplus
}
#endif

#endif
