// VeldraDB.cs - Core C# P/Invoke binding
// Part of the VeldraDB Unity Package (com.veldra.veldradb)
using System;
using System.Runtime.InteropServices;

namespace Veldra
{
    /// <summary>
    /// Low-level VeldraDB native bindings.
    /// Requires the native veldra.dll / libveldra.so / libveldra.dylib
    /// (included automatically by the package for supported platforms).
    /// </summary>
    public sealed class VeldraDB : IDisposable
    {
        private const string LibName = "veldra";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr veldra_init();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern IntPtr veldra_query(IntPtr db, string sql);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr veldra_result_get_text(IntPtr result);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void veldra_free_result(IntPtr result);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void veldra_close(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr veldra_get_last_error();

        private IntPtr _handle;
        private bool _disposed;

        /// <summary>Open a new VeldraDB instance.</summary>
        public VeldraDB()
        {
            _handle = veldra_init();
            if (_handle == IntPtr.Zero)
                throw new VeldraException("Failed to initialize VeldraDB: " + LastError());
        }

        /// <summary>Execute a query and return the result as a string.</summary>
        public string Query(string sql)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(VeldraDB));
            if (sql == null) throw new ArgumentNullException(nameof(sql));

            IntPtr resultPtr = veldra_query(_handle, sql);
            if (resultPtr == IntPtr.Zero)
                throw new VeldraException("Query failed: " + LastError());

            IntPtr textPtr = veldra_result_get_text(resultPtr);
            string output  = Marshal.PtrToStringAnsi(textPtr) ?? string.Empty;
            veldra_free_result(resultPtr);
            return output;
        }

        /// <summary>Returns the last native error string.</summary>
        public static string LastError()
        {
            IntPtr p = veldra_get_last_error();
            return p != IntPtr.Zero ? Marshal.PtrToStringAnsi(p) : "Unknown error";
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                if (_handle != IntPtr.Zero) { veldra_close(_handle); _handle = IntPtr.Zero; }
                _disposed = true;
            }
            GC.SuppressFinalize(this);
        }

        ~VeldraDB() => Dispose();
    }

    /// <summary>Exception thrown by VeldraDB operations.</summary>
    public class VeldraException : Exception
    {
        public VeldraException(string message) : base(message) { }
    }
}
