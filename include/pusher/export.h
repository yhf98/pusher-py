#ifndef PUSHER_EXPORT_H
#define PUSHER_EXPORT_H

#if defined(_WIN32) && defined(PUSHER_SHARED)
#  if defined(PUSHER_BUILDING_LIBRARY)
#    define PUSHER_API __declspec(dllexport)
#  else
#    define PUSHER_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(PUSHER_BUILDING_LIBRARY)
#  define PUSHER_API __attribute__((visibility("default")))
#else
#  define PUSHER_API
#endif

#endif
