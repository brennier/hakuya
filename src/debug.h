#ifndef HAKUYA_DEBUG_H
#define HAKUYA_DEBUG_H

#define PANIC(format, ...) do {						\
		fprintf(stderr, "Abort in %s() at %s:%d\n"		\
			"  " format "\n",				\
			__func__, __FILE__, __LINE__, __VA_ARGS__);	\
		abort();						\
	} while(0)

#endif // HAGKUYA_DEBUG_H
