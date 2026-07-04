// tray_unloader.cs — Unloads tray_resizer_dll.dll from explorer.exe
//
// This does the opposite of the injector: finds the loaded module
// in explorer.exe and calls FreeLibrary to unload it.
//
// Compile with:
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /platform:x64 tray_unloader.cs

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace TrayUnloader
{
    class Program
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes,
            uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter,
            uint dwCreationFlags, out uint lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern uint GetModuleFileName(IntPtr hModule, StringBuilder lpFilename, uint nSize);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool Module32First(IntPtr hSnapshot, ref MODULEENTRY32 lpme);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool Module32Next(IntPtr hSnapshot, ref MODULEENTRY32 lpme);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct MODULEENTRY32
        {
            public uint dwSize;
            public uint th32ModuleID;
            public uint th32ProcessID;
            public uint GlblcntUsage;
            public uint ProccntUsage;
            public IntPtr modBaseAddr;
            public uint modBaseSize;
            public IntPtr hModule;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
            public string szModule;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szExePath;
        }

        const uint PROCESS_CREATE_THREAD = 0x0002;
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint PROCESS_VM_OPERATION = 0x0008;
        const uint PROCESS_VM_WRITE = 0x0020;
        const uint PROCESS_VM_READ = 0x0010;
        const uint TH32CS_SNAPMODULE = 0x00000008;
        const uint INFINITE = 0xFFFFFFFF;

        static void Main(string[] args)
        {
            Console.WriteLine("Taskbar Tray Icon Resizer - Unloader");
            Console.WriteLine();

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

            // Open explorer.exe with necessary permissions
            IntPtr hProcess = OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                PROCESS_VM_OPERATION | PROCESS_VM_READ,
                false, pid);

            if (hProcess == IntPtr.Zero)
            {
                Console.Error.WriteLine("ERROR: Cannot open explorer.exe. Try running as Administrator.");
                Environment.Exit(1);
            }

            try
            {
                // Find our DLL module in explorer.exe using ToolHelp
                IntPtr hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
                if (hSnapshot == IntPtr.Zero || hSnapshot == new IntPtr(-1))
                {
                    Console.Error.WriteLine("ERROR: Could not create module snapshot.");
                    Environment.Exit(1);
                }

                MODULEENTRY32 me = new MODULEENTRY32();
                me.dwSize = (uint)Marshal.SizeOf(typeof(MODULEENTRY32));

                IntPtr hModule = IntPtr.Zero;
                string modName = "";

                if (Module32First(hSnapshot, ref me))
                {
                    do
                    {
                        if (me.szModule.IndexOf("tray_resizer_dll", StringComparison.OrdinalIgnoreCase) >= 0)
                        {
                            hModule = me.hModule;
                            modName = me.szModule;
                            break;
                        }
                    } while (Module32Next(hSnapshot, ref me));
                }
                CloseHandle(hSnapshot);

                if (hModule == IntPtr.Zero)
                {
                    Console.WriteLine("DLL not found in explorer.exe. It may not be loaded.");
                    Environment.Exit(0);
                }

                Console.WriteLine("Found loaded module: " + modName + " (0x" + hModule.ToString("X8") + ")");

                // Find FreeLibrary in kernel32.dll
                IntPtr kernel32 = GetModuleHandle("kernel32.dll");
                IntPtr freeLibrary = GetProcAddress(kernel32, "FreeLibrary");

                if (freeLibrary == IntPtr.Zero)
                {
                    Console.Error.WriteLine("ERROR: Could not find FreeLibrary.");
                    Environment.Exit(1);
                }

                // Create remote thread calling FreeLibrary(moduleHandle)
                uint threadId;
                IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0,
                    freeLibrary, hModule, 0, out threadId);

                if (hThread == IntPtr.Zero)
                {
                    Console.Error.WriteLine("ERROR: CreateRemoteThread failed. Try as Administrator.");
                    Environment.Exit(1);
                }

                WaitForSingleObject(hThread, INFINITE);
                CloseHandle(hThread);

                Console.WriteLine();
                Console.WriteLine("SUCCESS! DLL unloaded from explorer.exe.");
                Console.WriteLine("Tray icons will return to their default size.");
                Console.WriteLine();
                Console.WriteLine("You can now delete the DLL file.");
            }
            finally
            {
                CloseHandle(hProcess);
            }
        }
    }
}
