/*
* Copyright 2006 Sony Computer Entertainment Inc.
*
* Licensed under the MIT Open Source License, for details please see license.txt or the website
* http://www.opensource.org/licenses/mit-license.php
*
*/ 
// A home for commonly used utility functions. These are mostly for internal DOM
// use, but the automated tests use some of these functions, so we'll export
// them.
#ifndef daeUtils_h
#define daeUtils_h

#include <string>
#include <sstream>
#include <list>
#include <vector>
#include <dae/daePlatform.h>

namespace cdom {
	// System type info. We only need to distinguish between Posix and Winodws for now.
	enum systemType {
		Posix,
		Windows
	};

	// Get the system type at runtime.
	DLLSPEC systemType getSystemType();
	
	// String replace function. Usage: replace("abcdef", "cd", "12") --> "ab12ef".
	DLLSPEC std::string replace(const std::string& s, 
	                            const std::string& replace, 
	                            const std::string& replaceWith);

    // Removes whitespaces (" \t\f\v\n\r") at the beginning and the end of str.
    // If str consists of whitespaces only it will be erased.
    // Usage:
    //   trimWhitespaces("   a b") --> "a b"
    //   trimWhitespaces("a b   ") --> "a b"
    //   trimWhitespaces("   a b   ") --> "a b"
    //   trimWhitespaces("      ") --> ""
    DLLSPEC void trimWhitespaces(std::string& str);

	// Usage:
	//   tokenize("this/is some#text", "/#", true) --> ("this" "/" "is some" "#" "text")
	//   tokenize("this is some text", " ", false) --> ("this" "is" "some" "text")
	DLLSPEC std::list<std::string> tokenize(const std::string& s,
	                                        const std::string& separators,
	                                        bool separatorsInResult = false);
	// Same as the previous function, but returns the result via a parameter to avoid an object copy.
	DLLSPEC void tokenize(const std::string& s,
	                      const std::string& separators,
	                      /* out */ std::list<std::string>& tokens,
	                      bool separatorsInResult = false);

	typedef std::list<std::string>::iterator tokenIter;

//	DLLSPEC std::vector<std::string> makeStringArray(const char* s, ...);
//	DLLSPEC std::list<std::string> makeStringList(const char* s, ...);
	// degenerate makeString<T>(Container&, 0): end of recursion
	template < class Container, typename Type0 >
	void makeString(Container&, Type0 string0)
	{
		// All existing calls end with 0
		assert(! string0);
	}
	// makeString(Container&, at least one string, ..., 0)
	template < class Container,
			   typename Type0, typename Type1, typename ... Types >
	void makeString(Container& partial,
					Type0 string0, Type1 string1, Types... strings)
	{
		partial.push_back(string0);
		makeString(partial, string1, strings...);
	}
	template < typename ... Types >
	std::vector<std::string> makeStringArray(Types... strings)
	{
		std::vector<std::string> result;
		makeString(result, strings...);
		return result;
	}
	template < typename ... Types >
	std::list<std::string> makeStringList(Types... strings)
	{
		std::list<std::string> result;
		makeString(result, strings...);
		return result;
	}

	DLLSPEC std::string getCurrentDir();
	DLLSPEC std::string getCurrentDirAsUri();

    // Returns platform specific file separator.
    // \ on windows
    // / on other platforms
    DLLSPEC char getFileSeparator();

#ifndef NO_BOOST
    // Returns system wide temporary directory.
    // Reads environment variable TMP.
    DLLSPEC const std::string& getSystemTmpDir();

    // Returns a filename obtained via tmpnam().
    // On systems where tmpnam()'s result is preceded
    // with a directory, that directory is cutoff.
    DLLSPEC std::string getRandomFileName();

    // Returns getSystemTmpDir() appended with a randomly
    // generated directory name.
    // This directory will be deleted when DAE gets destroyed.
    DLLSPEC const std::string& getSafeTmpDir();
#endif //NO_BOOST

    DLLSPEC int strcasecmp(const char* str1, const char* str2);
	DLLSPEC std::string tolower(const std::string& s);

	// Disable VS warning
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)
#endif
	template<typename T>
	std::string toString(const T& val) {
		std::ostringstream stream;
		stream << val;
		return stream.str();
	}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

#endif
