// tray_resizer.cs - C# injector for tray_resizer_dll.dll
// 
// Compiles with the built-in csc.exe (no Visual Studio needed):
//   "%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /platform:x64 tray_resizer.cs
//
// This finds explorer.exe and injects tray_resizer_dll.dll into it.
// The DLL must be in the same directory as this executable.

using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace TrayResizer
{
    class Program
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress,
            uint dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress,
            uint dwSize, uint dwFreeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress,
            byte[] lpBuffer, uint nSize, out uint lpNumberOfBytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes,
            uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter,
            uint dwCreationFlags, out uint lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetExitCodeThread(IntPtr hThread, out IntPtr lpExitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out long lpLuid);

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges,
            ref TOKEN_PRIVILEGES NewState, uint BufferLength, IntPtr PreviousState, IntPtr ReturnLength);

        [StructLayout(LayoutKind.Sequential)]
        struct TOKEN_PRIVILEGES
        {
            public uint PrivilegeCount;
            public long Luid;
            public uint Attributes;
        }

        const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
        const uint TOKEN_QUERY = 0x0008;
        const uint SE_PRIVILEGE_ENABLED = 0x00000002;
        const string SE_DEBUG_NAME = "SeDebugPrivilege";

        const uint PROCESS_CREATE_THREAD = 0x0002;
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint PROCESS_VM_OPERATION = 0x0008;
        const uint PROCESS_VM_WRITE = 0x0020;
        const uint PROCESS_VM_READ = 0x0010;

        const uint MEM_COMMIT = 0x1000;
        const uint MEM_RESERVE = 0x2000;
        const uint MEM_RELEASE = 0x8000;
        const uint PAGE_READWRITE = 0x04;

        const uint INFINITE = 0xFFFFFFFF;

        static void EnableDebugPrivilege()
        {
            IntPtr token;
            if (OpenProcessToken(Process.GetCurrentProcess().Handle,
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out token))
            {
                long luid;
                if (LookupPrivilegeValue(null, SE_DEBUG_NAME, out luid))
                {
                    TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
                    tp.PrivilegeCount = 1;
                    tp.Luid = luid;
                    tp.Attributes = SE_PRIVILEGE_ENABLED;
                    AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
                }
                CloseHandle(token);
            }
        }

        static void Main(string[] args)
        {
            Console.WriteLine("Taskbar Tray Icon Resizer v1.0");
            Console.WriteLine("Icon size: 24px (compile-time configurable in the DLL)");
            Console.WriteLine();

            // Get DLL path (same directory as EXE)
            string dllPath = Path.Combine(
                Path.GetDirectoryName(typeof(Program).Assembly.Location) ?? ".",
                "tray_resizer_dll.dll");

            if (!File.Exists(dllPath))
            {
                Console.Error.WriteLine("ERROR: DLL not found at: " + dllPath);
                Console.Error.WriteLine("Make sure tray_resizer_dll.dll is in the same directory.");
                Console.Error.WriteLine();
                Console.Error.WriteLine("To build the DLL, you need Visual Studio 2022+ with C++ support.");
                Console.Error.WriteLine("Open a 'x64 Native Tools Command Prompt' and run:");
                Console.Error.WriteLine("  build.bat");
                Environment.Exit(1);
            }

            // Enable debug privilege
            EnableDebugPrivilege();

            // Find explorer.exe
            uint pid = 0;
            foreach (var proc in Process.GetProcessesByName("explorer"))
            {
                pid = (uint)proc.Id;
                break;
            }

            if (pid == 0)
            {
                Console.Error.WriteLine("ERROR: Could not find explorer.exe process.");
                Environment.Exit(1);
            }

            Console.WriteLine("Found explorer.exe (PID: " + pid + ")");

            // Open explorer.exe
            IntPtr hProcess = OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                false, pid);

            if (hProcess == IntPtr.Zero)
            {
                int err = Marshal.GetLastWin32Error();
                Console.Error.WriteLine("ERROR: Cannot open explorer.exe (error " + err + ").");
                Console.Error.WriteLine("Try running this program as Administrator.");
                Environment.Exit(1);
            }

            try
            {
                // Allocate memory in explorer.exe for the DLL path
                byte[] dllPathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
                uint pathSize = (uint)dllPathBytes.Length;

                IntPtr remoteMem = VirtualAllocEx(hProcess, IntPtr.Zero,
                    pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

                if (remoteMem == IntPtr.Zero)
                {
                    int err = Marshal.GetLastWin32Error();
                    Console.Error.WriteLine("ERROR: VirtualAllocEx failed (error " + err + ").");
                    Environment.Exit(1);
                }

                try
                {
                    // Write DLL path into target memory
                    uint written;
                    if (!WriteProcessMemory(hProcess, remoteMem, dllPathBytes, pathSize, out written))
                    {
                        int err = Marshal.GetLastWin32Error();
                        Console.Error.WriteLine("ERROR: WriteProcessMemory failed (error " + err + ").");
                        Environment.Exit(1);
                    }

                    // Find LoadLibraryW in kernel32.dll
                    IntPtr kernel32 = GetModuleHandle("kernel32.dll");
                    IntPtr loadLibraryW = GetProcAddress(kernel32, "LoadLibraryW");

                    if (loadLibraryW == IntPtr.Zero)
                    {
                        Console.Error.WriteLine("ERROR: Could not find LoadLibraryW.");
                        Environment.Exit(1);
                    }

                    // Create remote thread calling LoadLibraryW(dllPath)
                    uint threadId;
                    IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0,
                        loadLibraryW, remoteMem, 0, out threadId);

                    if (hThread == IntPtr.Zero)
                    {
                        int err = Marshal.GetLastWin32Error();
                        Console.Error.WriteLine("ERROR: CreateRemoteThread failed (error " + err + ").");
                        Console.Error.WriteLine("Try running as Administrator.");
                        Environment.Exit(1);
                    }

                    // Wait for thread completion
                    WaitForSingleObject(hThread, INFINITE);

                    // Check if LoadLibrary succeeded
                    IntPtr exitCode;
                    GetExitCodeThread(hThread, out exitCode);

                    CloseHandle(hThread);

                    if (exitCode == IntPtr.Zero)
                    {
                        Console.Error.WriteLine("ERROR: LoadLibraryW returned NULL - DLL injection failed.");
                        Console.Error.WriteLine("Check that the DLL is compatible with this Windows version.");
                        Environment.Exit(1);
                    }

                    Console.WriteLine();
                    Console.WriteLine("SUCCESS! Tray icon resizer injected into explorer.exe.");
                    Console.WriteLine("Icons will be resized to 24px within a few seconds.");
                    Console.WriteLine("The resizer will reapply styles periodically for new icons.");
                    Console.WriteLine();
                    Console.WriteLine("To stop: restart explorer.exe, or log out and back in.");
                }
                finally
                {
                    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
                }
            }
            finally
            {
                CloseHandle(hProcess);
            }
        }
    }
}
