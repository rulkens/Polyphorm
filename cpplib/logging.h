#pragma once
#include <stdint.h>

// Cast needed: strrchr(...) yields char*, __FILE__ decays to const char*; without
// the cast the ternary's common type is const char*, which can't bind to
// print_error_with_location's `char *filename` parameter (found in Task 5 while
// wiring up main.cpp's first real logging::print_error call site).
#define __FILENAME__ (char *)(strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

#define print_error(format, ...) print_error_with_location(format, __FILENAME__, __LINE__, __VA_ARGS__)

namespace logging
{
	// General purpose print
	void print(char *format, ...);

	// Prints error message along with filename and line number.
	// Example:
	// "ERROR in file %FILENAME on line %LINE_NUMBER: "
	// %ERROR_MESSAGE
	void print_error_with_location(char *format, char *filename, uint32_t line, ...);
}