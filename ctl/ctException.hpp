/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// CPP headers
#include <malloc.h>
#include <memory.h>
#include <intrin.h>
#include <string>
#include <exception>
// OS headers
#include <Windows.h>


namespace ctl {

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //  ctException
    //
    //  Derived from the base class of exception.
    //
    //  A class to provide deeper exception details including:
    //    - a wide-char version of exception::what()
    //    - an optional unsigned long variable to store a numeric error code
    //  - an optional wide-char string for the location of the exception throwing
    //  - a method to translate Win32 error codes to a wide-char string
    //
    //  All methods are specified throw() - no method can fail
    //  - should alloc failures occur, an empty string is returned the the caller
    //
    //  ** Note on methods returning pointers to const buffers:
    //  Just as with the wstring::c_str() and string::c_str() methods, as well as the
    //    virtual method exception::what() that class this overrides:
    //    The semantics of the pointers returned:
    //      - must remain const (modifying them can and will corrupt the internals of the object)
    //      - only survive the lifetime of the object
    //  Where the semantics differ from the wstring::c_str() and string::c_str() methods
    //    is that once the const pointer is created, it is guaranteed not to change 
    //    for the lifetime of the ctException object.
    //
    //  Note: the following guards areas of this file which are only available for desktop apps
    //        modern apps do not have access to this functionality
    //
    //    #pragma region Desktop Family
    //    #if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    class ctException : public std::exception {
    public:
        // constructors
        explicit ctException(unsigned long _ulCode) throw();
        explicit ctException(_In_ LPCSTR _wszMessage, bool _bMessageCopy = true) throw();
        explicit ctException(_In_ LPCWSTR _wszMessage, bool _bMessageCopy = true) throw();
        explicit ctException(_In_ const std::wstring& _wsMessage) throw();
        explicit ctException(_In_ const std::string& _sMessage) throw();

        explicit ctException(unsigned long _ulCode, _In_ LPCWSTR _wszMessage, bool _bMessageCopy = true) throw();
        explicit ctException(unsigned long _ulCode, _In_ LPCWSTR _wszMessage, _In_ LPCWSTR _wszLocation, bool _bBothStringCopy = true) throw();
        explicit ctException(unsigned long _ulCode, _In_ LPCSTR _szMessage, bool _bMessageCopy = true) throw();
        explicit ctException(unsigned long _ulCode, _In_ const std::wstring& _wsMessage) throw();
        explicit ctException(unsigned long _ulCode, _In_ const std::string& _sMessage) throw();

        ctException() throw();
        ctException(_In_ const std::exception& e) throw();
        ctException(_In_ const ctException& e) throw();

        // operator= implementations
        virtual ctException& operator=(_In_ const std::exception& e) throw();
        virtual ctException& operator=(_In_ const ctException& e) throw();

        // destructor
        virtual ~ctException() throw();

        // public accessors
        virtual const unsigned long why() const throw();
        virtual const char* what() const throw();
        virtual const wchar_t* what_w() const throw();
        virtual const wchar_t* where_w() const throw();

        virtual const wchar_t* translation_w() const throw();

        // no-fail operators
        virtual void reset() throw();

    private:
        // private functions to alloc and copy string buffers
        char* calloc_and_copy_s(_In_ LPCSTR tSrc) const throw();
        wchar_t* calloc_and_copy_w(_In_ LPCWSTR tSrc) const throw();

        // not making swap() public since the base class does not implement swap()
        // - and that risks getting our objects out of sync
        // - using this internally in a controlled manner with temporaries
        void swap(ctException& e) throw();

        // private multi-byte/wide-char translators
        void wszMessageToszMessage() throw();
        void szMessageTowszMessage() throw();

        // private member variables
        unsigned long ulCode;
        const char* szMessage;
        const wchar_t* wszMessage;
        const wchar_t* wszLocation;
        mutable wchar_t* wszTranslation;
        bool bMessageCopy_s;
        bool bMessageCopy_w;
        bool bLocationCopy_w;
    };

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // Constructors
    //
    // - all provide a best attempt at preserving the original error strings
    // - allows char* strings for the "message" to maintain compatibility with exception::what()
    // - allows for an optional bool value to be passed to specify whether to make a copy of the string
    //   - if no copy is made, the pointer won't be deleted on destruction
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline ctException::ctException() throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(0),
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(false),
        bLocationCopy_w(false)
    {
    }

    inline ctException::ctException(unsigned long _ulCode) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(_ulCode), // initializing with args
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(false),
        bLocationCopy_w(false)
    {
    }

    inline ctException::ctException(_In_ LPCSTR _szMessage, bool _bMessageCopy) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(0),
        szMessage(_szMessage), // initializing with args
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(_bMessageCopy),  // initializing with args
        bMessageCopy_w(false),
        bLocationCopy_w(false)
    {
        if (bMessageCopy_s) {
            szMessage = calloc_and_copy_s(_szMessage);
        }
        // build the corresponding wchar_t string
        szMessageTowszMessage();
    }

    inline ctException::ctException(_In_ LPCWSTR _wszMessage, bool _bMessageCopy) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(0),
        szMessage(nullptr),
        wszMessage(_wszMessage), // initializing with args
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(_bMessageCopy), // initializing with args
        bLocationCopy_w(false)
    {
        if (bMessageCopy_w) {
            wszMessage = calloc_and_copy_w(_wszMessage);
        }
        // build the corresponding char string
        wszMessageToszMessage();
    }

    inline ctException::ctException(_In_ const std::wstring& _wsMessage) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(0),
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(true),  // always copy out of the std::wstring
        bLocationCopy_w(false)
    {
        wszMessage = calloc_and_copy_w(_wsMessage.c_str());
        // build the corresponding char string
        wszMessageToszMessage();
    }

    inline ctException::ctException(_In_ const std::string& _sMessage) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(0),
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(true),  // always copy out of the string
        bMessageCopy_w(false),
        bLocationCopy_w(false)
    {
        const char* szMessageString = nullptr;
        try {
            szMessageString = _sMessage.c_str();
        }
        catch (const std::exception&) {
            szMessageString = nullptr;
        }

        if (szMessageString != nullptr) {
            szMessage = calloc_and_copy_s(szMessageString);
            // build the corresponding wchar_t string
            szMessageTowszMessage();
        }
    }

    inline ctException::ctException(unsigned long _ulCode, _In_ LPCWSTR _wszMessage, bool _bMessageCopy) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(_ulCode), // initializing with args
        szMessage(nullptr),
        wszMessage(_wszMessage), // initializing with args
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(_bMessageCopy), // initializing with args
        bLocationCopy_w(false)
    {
        if (bMessageCopy_w) {
            wszMessage = calloc_and_copy_w(_wszMessage);
        }
        // build the corresponding char string
        wszMessageToszMessage();
    }

    inline ctException::ctException(unsigned long _ulCode, _In_ LPCWSTR _wszMessage, _In_ LPCWSTR _wszLocation, bool _bBothStringCopy) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(_ulCode),
        szMessage(nullptr),
        wszMessage(_wszMessage), // initializing with args
        wszLocation(_wszLocation), // initializing with args
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(_bBothStringCopy), // initializing with args
        bLocationCopy_w(_bBothStringCopy) // initializing with args
    {
        if (_bBothStringCopy) {
            wszMessage = calloc_and_copy_w(_wszMessage);
            wszLocation = calloc_and_copy_w(_wszLocation);
        }
        // build the corresponding char string
        wszMessageToszMessage();
    }

    inline ctException::ctException(unsigned long _ulCode, _In_ LPCSTR _szMessage, bool _bMessageCopy) throw()
    : std::exception(_szMessage), // initialize the base c'tor
        ulCode(_ulCode), // initializing with args
        szMessage(_szMessage), // initializing with args
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(_bMessageCopy), // initializing with args
        bMessageCopy_w(false),
        bLocationCopy_w(false)
    {
        if (bMessageCopy_s) {
            szMessage = calloc_and_copy_s(_szMessage);
        }
        // build the corresponding wchar_t string
        szMessageTowszMessage();
    }

    inline ctException::ctException(unsigned long _ulCode, _In_ const std::wstring& _wsMessage) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(_ulCode), // initializing with args
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(false),
        bMessageCopy_w(true),  // always copy out of the std::wstring
        bLocationCopy_w(false)
    {
        const wchar_t* wszMessageString = nullptr;
        try {
            wszMessageString = _wsMessage.c_str();
        }
        catch (const std::exception&) {
            wszMessageString = nullptr;
        }

        if (wszMessageString != nullptr) {
            wszMessage = calloc_and_copy_w(wszMessageString);
            // build the corresponding char string
            wszMessageToszMessage();
        }
    }

    inline ctException::ctException(unsigned long _ulCode, _In_ const std::string& _sMessage) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(_ulCode), // initializing with args
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(true),  // always copy out of the string
        bMessageCopy_w(false),
        bLocationCopy_w(false)
    {
        const char* szMessageString = nullptr;
        try {
            szMessageString = _sMessage.c_str();
        }
        catch (const std::exception&) {
            szMessageString = nullptr;
        }

        if (szMessageString != nullptr) {
            szMessage = calloc_and_copy_s(szMessageString);
            // build the corresponding wchar_t string
            szMessageTowszMessage();
        }
    }

    inline ctException::ctException(_In_ const std::exception& e) throw()
    : std::exception(e), // initialize the base c'tor
        ulCode(0),
        szMessage(nullptr),
        wszMessage(nullptr),
        wszLocation(nullptr),
        wszTranslation(nullptr),
        bMessageCopy_s(true),  // always copy what() out of the std::exception
        bMessageCopy_w(true),
        bLocationCopy_w(false)
    {
        szMessage = calloc_and_copy_s(e.what());
        // build the corresponding wchar_t string
        szMessageTowszMessage();
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // Copy constructor
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline ctException::ctException(_In_ const ctException& e) throw()
    : std::exception(""), // initialize the base c'tor
        ulCode(e.ulCode),
        szMessage(e.szMessage),
        wszMessage(e.wszMessage),
        wszLocation(e.wszLocation),
        wszTranslation(nullptr), // don't copy this buffer allocated by FormatString
        bMessageCopy_s(e.bMessageCopy_s),
        bMessageCopy_w(e.bMessageCopy_w),
        bLocationCopy_w(e.bLocationCopy_w)
    {
        if (bMessageCopy_s) {
            szMessage = calloc_and_copy_s(e.szMessage);
        }

        if (bMessageCopy_w) {
            wszMessage = calloc_and_copy_w(e.wszMessage);
        }

        if (bLocationCopy_w) {
            wszLocation = calloc_and_copy_w(e.wszLocation);
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // operator= assignment
    // - safe even if (this == &e)
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline ctException& ctException::operator=(_In_ const std::exception& e) throw()
    {
        //
        // Make a copy before setting local variables
        //
        ctException ctTemp(e);
        //
        // swap and return
        //
        this->swap(ctTemp);
        return *this;
    }
    inline ctException& ctException::operator=(_In_ const ctException& e) throw()
    {
        //
        // Make a copy before setting local variables
        //
        ctException ctTemp(e);
        //
        // swap and return
        //
        this->swap(ctTemp);
        return *this;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // Destructor
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline ctException::~ctException() throw()
    {
        this->reset();
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // reset()
    // - no-fail operation to clear all members
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline void ctException::reset() throw()
    {
        if (this->bMessageCopy_s)  ::free(const_cast<void*>(static_cast<const void*>(this->szMessage)));
        if (this->bMessageCopy_w)  ::free(const_cast<void*>(static_cast<const void*>(this->wszMessage)));
        if (this->bLocationCopy_w) ::free(const_cast<void*>(static_cast<const void*>(this->wszLocation)));

        // always allocated by FormatMessage
#pragma region Desktop Family
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        ::LocalFree(static_cast<void*>(this->wszTranslation));
#endif
#pragma endregion

        this->ulCode = 0;
        this->szMessage = nullptr;
        this->wszMessage = nullptr;
        this->wszLocation = nullptr;
        this->wszTranslation = nullptr;
        this->bMessageCopy_s = false;
        this->bMessageCopy_w = false;
        this->bLocationCopy_w = false;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // why()
    // - returns the unsigned long integer error code
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline const unsigned long ctException::why() const throw()
    {
        return ulCode;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // what()
    // - returns the char* member for the reason for the failure
    // - virtual function called if called polymorphically from an std::exception object
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline const char * ctException::what() const throw()
    {
        return (this->szMessage != nullptr) ? this->szMessage : "";
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // what_w()
    // - returns the wchar_t* member for the reason for the failure
    // - the "wide" version of std::exception::what()
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline const wchar_t* ctException::what_w() const throw()
    {
        return (this->wszMessage != nullptr) ? this->wszMessage : L"";
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // where_w()
    // - returns the wchar_t* member for where the failure occured
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline const wchar_t* ctException::where_w() const throw()
    {
        return (this->wszLocation != nullptr) ? this->wszLocation : L"";
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // translation_w()
    // - returns the wchar_t* member for the system translation of the error from this->why()
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline const wchar_t* ctException::translation_w() const throw()
    {
#pragma region Desktop Family
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        static const DWORD DWFLAGS =
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS |
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_MAX_WIDTH_MASK;

        void* lpBuffer = nullptr;
        if (nullptr == this->wszTranslation) {
            DWORD dwReturn = ::FormatMessageW(
                DWFLAGS,
                nullptr, // just search the system
                this->ulCode,
                0, // allow for proper MUI language fallback
                reinterpret_cast<LPWSTR>(&lpBuffer),
                0,
                nullptr
                );
            if (dwReturn != 0) {
                // if 0, couldn't find the resource
                this->wszTranslation = reinterpret_cast<wchar_t*>(lpBuffer);
            }
        }
#endif
#pragma endregion

        // modern apps will always have "" returned
        return (this->wszTranslation != nullptr) ? this->wszTranslation : L"";
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // wszMessageToszMessage()
    // - converts the wide-char message string to the multi-byte char string
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline void ctException::wszMessageToszMessage() throw()
    {
        if (nullptr == this->wszMessage) {
            return;
        }

        int iResult = ::WideCharToMultiByte(
            CP_ACP,
            WC_NO_BEST_FIT_CHARS,
            this->wszMessage,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr
            );
        if (iResult != 0) {
            char* temp_szMessage = static_cast<char*>(calloc(iResult, sizeof(char)));
            if (temp_szMessage != nullptr) {
                iResult = ::WideCharToMultiByte(
                    CP_ACP,
                    WC_NO_BEST_FIT_CHARS,
                    this->wszMessage,
                    -1,
                    temp_szMessage,
                    iResult,
                    nullptr,
                    nullptr
                    );
                if (0 == iResult) {
                    // failed to populate the buffer, so should free it
                    free(temp_szMessage);
                } else {
                    this->szMessage = temp_szMessage;
                    this->bMessageCopy_s = true;
                }
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // szMessageTowszMessage()
    // - converts the multi-byte char message string to the wide-char string
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline void ctException::szMessageTowszMessage() throw()
    {
        if (nullptr == this->szMessage) {
            return;
        }

        int iResult = ::MultiByteToWideChar(
            CP_ACP,
            0,
            this->szMessage,
            -1,
            nullptr,
            0
            );
        if (iResult != 0) {
            wchar_t* temp_wszMessage = static_cast<wchar_t*>(calloc(iResult, sizeof(wchar_t)));
            if (temp_wszMessage != nullptr) {
                iResult = ::MultiByteToWideChar(
                    CP_ACP,
                    0,
                    this->szMessage,
                    -1,
                    temp_wszMessage,
                    iResult
                    );
                if (0 == iResult) {
                    // failed to populate the buffer, so should free it
                    free(temp_wszMessage);
                } else {
                    this->wszMessage = temp_wszMessage;
                    this->bMessageCopy_w = true;
                }
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // calloc_and_copy_s(char* )
    //
    // - allocates a buffer and copies the multi-byte char string
    // - returns the pointer to the allocated buffer
    //        - or nullptr if failed to allocate
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline char* ctException::calloc_and_copy_s(_In_ LPCSTR tSrc) const throw()
    {
        // no need to copy a nullptr ptr
        if (nullptr == tSrc) {
            return nullptr;
        }

        size_t tSize = strlen(tSrc) + 1;
        char* tDest = static_cast<char*>(calloc(tSize, sizeof(char)));
        if (tDest != nullptr) {
            errno_t err = strcpy_s(tDest, tSize, tSrc);
            if (err != 0) {
                free(tDest);
                tDest = nullptr;
            }
        }

        return tDest;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // calloc_and_copy_w(wchar_t* )
    //
    // - allocates a buffer and copies the wide-char string
    // - returns the pointer to the allocated buffer
    //        - or nullptr if failed to allocate
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline wchar_t* ctException::calloc_and_copy_w(_In_ LPCWSTR tSrc) const throw()
    {
        // no need to copy a nullptr ptr
        if (nullptr == tSrc) {
            return nullptr;
        }

        size_t tSize = wcslen(tSrc) + 1;
        wchar_t* tDest = static_cast<wchar_t*>(calloc(tSize, sizeof(wchar_t)));
        if (tDest != nullptr) {
            errno_t err = wcscpy_s(tDest, tSize, tSrc);
            if (err != 0) {
                free(tDest);
                tDest = nullptr;
            }
        }

        return tDest;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // swap()
    // - no-fail swap operation to swap 2 ctException objects
    //
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    inline void ctException::swap(ctException& e) throw()
    {
        using std::swap;
        swap(this->ulCode, e.ulCode);
        swap(this->szMessage, e.szMessage);
        swap(this->wszMessage, e.wszMessage);
        swap(this->wszLocation, e.wszLocation);
        swap(this->wszTranslation, e.wszTranslation);
        swap(this->bMessageCopy_s, e.bMessageCopy_s);
        swap(this->bMessageCopy_w, e.bMessageCopy_w);
        swap(this->bLocationCopy_w, e.bLocationCopy_w);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // ctFatalCondition
    //
    // Functions which serve as an "assert" to RaiseException should a condition be false.
    // Useful to when expressing invariants in ones code to make them more easily debuggable.
    // The relevant string details of the failed condition is written to the debugger before it breaks.
    // The string is additionally written to the STDERR file handle - if a console is waiting for it
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // NTSTATUS being used: 1110 (E) for severity, c71f00d for facility & code (ctl)
    static const unsigned long ctFatalConditionExceptionCode = 0xec71f00d;
    namespace {
        ///
        /// the call to RaiseFailFastException is put into its own function
        ///  - so it can be described __analysis_noreturn for SAL
        ///
        __analysis_noreturn
        void FailFast(_In_ EXCEPTION_RECORD* _exr)
        {
            ::RaiseFailFastException(_exr, nullptr, 0);
        }
    }
    inline
    void ctFatalConditionVa(bool _condition, _Printf_format_string_ const wchar_t* _text, _In_ va_list _argptr) throw()
    {
        if (_condition) {
            // write everyting out into a single string
            static const size_t exception_length = 512;
            wchar_t exception_text[exception_length] = { L'\0' };
            ::_vsnwprintf_s(exception_text, exception_length, _TRUNCATE, _text, _argptr);

            // first print it to the output stream
            ::fwprintf_s(stderr, exception_text);

            // then write it out to the debugger
            ::OutputDebugString(exception_text);

            // then raise the exception to break
            EXCEPTION_RECORD exr;
            ::ZeroMemory(&exr, static_cast<DWORD>(sizeof(exr)));
            exr.ExceptionCode = ctFatalConditionExceptionCode;
            exr.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
            exr.ExceptionRecord = nullptr;
            exr.ExceptionAddress = _ReturnAddress();
            exr.NumberParameters = 1;
            exr.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(exception_text);
            FailFast(&exr);
        }
    }
    inline
    __analysis_noreturn
    void ctAlwaysFatalConditionVa(_Printf_format_string_ const wchar_t* _text, _In_ va_list _argptr) throw()
    {
        ctFatalConditionVa(true, _text, _argptr);
    }
    inline
    void ctFatalCondition(bool _condition, _Printf_format_string_ const wchar_t* _text, ...) throw()
    {
        if (_condition) {
            va_list argptr;
            va_start(argptr, _text);
            ctFatalConditionVa(_condition, _text, argptr);
            va_end(argptr);
        }
    }
    inline
    __analysis_noreturn
    void ctAlwaysFatalCondition(_Printf_format_string_ const wchar_t* _text, ...) throw()
    {
        va_list argptr;
        va_start(argptr, _text);
        ctFatalConditionVa(true, _text, argptr);
        va_end(argptr);
    }
    ///
    /// If enabled Runtime Type Information, will detect which exception based off the actual type
    /// - this is the /GR flag to the compiler
    /// - in the sources makefile: USE_RTTI = 1
    ///
    inline
    __analysis_noreturn
    void ctFatalCondition(_In_ const ctException& _exception) throw()
    {
        ctFatalCondition(
            true,
            L"ctException : %s at %s [%u / 0x%x - %s]",
            _exception.what_w(),
            _exception.where_w(),
            _exception.why(),
            _exception.why(),
            _exception.translation_w());
    }

#ifdef _CPPRTTI
    inline
    __analysis_noreturn
    void ctFatalCondition(_In_ const std::exception& _exception) throw()
    {
        const ctException* ctex = dynamic_cast<const ctException*>(&_exception);
        if (ctex != nullptr) {
            ctFatalCondition(*ctex);
        } else {
            ctFatalCondition(
                true,
                L"std::exception : %S",
                _exception.what());
        }
    }
#else
    inline
    __analysis_noreturn
    void ctFatalCondition(_In_ const std::exception& _exception) throw()
    {
        ctFatalCondition(
            true,
            L"std::exception : %S",
            _exception.what());
    }
#endif

}  // namespace ctl

