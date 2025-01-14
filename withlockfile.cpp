/* withlockfile - a program for synchronising command execution
 * Copyright 2014-2017 froglogic GmbH
 *
 * This file is part of withlockfile.
 *
 * withlockfile is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * withlockfile is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with withlockfile.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <iostream>
#include <stdexcept>

#include <windows.h>
#include <shlwapi.h>

class Win32Error : public std::runtime_error
{
public:
    Win32Error( const char *what, DWORD errorCode_ )
        : std::runtime_error( what )
        , errorCode( errorCode_ )
    {
    }

    const DWORD errorCode;
};

static std::string enforceExeExtension( const std::string &s )
{
    std::string exe = s;
    const std::string::size_type len = exe.size();
    if ( len < 4 ||
         _stricmp( exe.substr( len - 4 ).c_str(), ".exe" ) != 0 ) {
        exe += ".exe";
    }
    return exe;
}

static std::string quoteArgument( std::string arg )
{
    const std::string::size_type space = arg.find_first_of( " \t" );
    if ( space != std::string::npos ) {
        std::string s = "\"";
        s += arg;
        s += '"';
        return s;
    }
    return arg;
}

int main( int argc, char **argv )
{
    try {
        if ( argc < 3 ) {
            std::cerr << "usage: withlockfile <lockfile> <command> [args..]\n";
            return 1;
        }

        HANDLE f = ::CreateFileA( argv[1],
                                  GENERIC_READ, /* required by LockFileEx */
                                  FILE_SHARE_READ, /* allow concurrent opening */
                                  NULL,
                                  OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_READONLY,
                                  NULL );
        if ( f == INVALID_HANDLE_VALUE ) {
            throw Win32Error( "CreateFileA", ::GetLastError() );
        }

        OVERLAPPED ol;
        ::ZeroMemory( &ol, sizeof( ol ) );

        bool lockAcquired = false;
        for ( int i = 0; i < 300; ++i ) {
            if ( ::LockFileEx( f,
                               LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                               0,
                               1,
                               0,
                               &ol ) ) {
                lockAcquired = true;
                break;
            }

            const DWORD lastError = ::GetLastError();
            if ( lastError == ERROR_LOCK_VIOLATION ) {
                ::Sleep( 1000 );
                continue;
            } else {
                throw Win32Error( "LockFileEx", lastError );
            }
        }

        if ( !lockAcquired ) {
            throw Win32Error( "LockFileEx", ERROR_LOCK_VIOLATION );
        }

        std::string executable = enforceExeExtension( argv[2] );
        {
            // According to a comment on the PathSearchAndQualify function
            // at http://msdn.microsoft.com/en-us/library/bb773751(VS.85).aspx
            // the buffer must be at least MAX_PATH in size.
            char buf[MAX_PATH] = { 0 };
            if ( !::PathSearchAndQualifyA( executable.c_str(),
                                           buf, sizeof( buf ) ) ) {
                throw Win32Error( "PathSearchAndQualifyA", ::GetLastError() );
            }
            executable = buf;
        }

        std::string commandLine = quoteArgument( executable );
        {
            for ( int i = 3; i < argc; ++i ) {
                commandLine += ' ';
                commandLine += quoteArgument( argv[i] );
            }
        }

        PROCESS_INFORMATION pi = { 0 };

        STARTUPINFOA sia = { sizeof(sia)};
        sia.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        sia.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        sia.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);


        if ( ::CreateProcessA( executable.c_str(),
                               const_cast<char *>( commandLine.c_str() ),
                               NULL,
                               NULL,
                               TRUE,
                               CREATE_SUSPENDED,
                               NULL,
                               NULL,
                               &sia,
                               &pi ) == FALSE ) {
            throw Win32Error( "CreateProcessA", ::GetLastError() );
        }

        HANDLE jobObject = ::CreateJobObject( NULL, NULL );
        if ( !jobObject ) {
            throw Win32Error( "CreateJobObject", ::GetLastError() );
        }

        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobObjectInfo = { 0 };
            jobObjectInfo.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if ( !::SetInformationJobObject( jobObject,
                                             JobObjectExtendedLimitInformation,
                                             &jobObjectInfo,
                                             sizeof( jobObjectInfo ) ) ) {
                ::CloseHandle( jobObject );
                throw Win32Error( "SetInformationJobObject", ::GetLastError() );
            }
        }

        /* Don't bother reporting access denied with AssignProcessToJobObject
         * because it's quite common for this to happen on Windows 7 and
         * earlier if withlockfile is already part of a job object.
         */
        if ( !::AssignProcessToJobObject( jobObject, pi.hProcess ) &&
             ::GetLastError() != ERROR_ACCESS_DENIED ) {
            throw Win32Error( "AssignProcessToJobObject", ::GetLastError() );
        }

        if ( ::ResumeThread( pi.hThread ) == -1 ) {
            throw Win32Error( "ResumeThread", ::GetLastError() );
        }

        if ( ::WaitForSingleObject( pi.hProcess, INFINITE ) == WAIT_FAILED ) {
            throw Win32Error( "WaitForSingleObject", ::GetLastError() );
        }

        DWORD exitCode;
        if ( ::GetExitCodeProcess( pi.hProcess, &exitCode ) == FALSE ) {
            throw Win32Error( "GetExitCodeProcess", ::GetLastError() );
        }

        if ( ::UnlockFileEx( f, 0, 1, 0, &ol ) == FALSE ) {
            throw Win32Error( "UnlockFileEx", ::GetLastError() );
        }

        if ( ::CloseHandle( f ) == FALSE ) {
            throw Win32Error( "CloseHandle", ::GetLastError() );
        }

        return exitCode;
    } catch ( const Win32Error &e ) {
        /* The MSDN documentation for FormatMessage says that the buf cannot
         * be larger than 64K bytes.
         */
        char buf[65536 / sizeof(wchar_t)] = { L'0' };

        const DWORD result = ::FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            e.errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            buf,
            sizeof( buf ),
            NULL );

        /* It seems that many error messages end in a newline, but I don't want
         * them, so strip them.
         */
        const size_t len = strlen( buf );
        if ( len > 1 && buf[len - 2] == '\r' && buf[len - 1] == '\n' ) {
            buf[len - 2] = '\0';
        }

        std::cerr << "error: " << e.what() << " failed: "
            << buf << " (code " << e.errorCode << ")"
            << std::endl;

        return e.errorCode;
    } catch ( const std::exception &e ) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

