// FluxDB.cs - Core C# P/Invoke binding
// Part of the FluxDB Unity Package (com.flux.fluxdb)
using System;
using System.Runtime.InteropServices;

namespace Flux
{
    /// <summary>
    /// Low-level FluxDB native bindings.
    /// Requires the native flux.dll / libflux.so / libflux.dylib
    /// (included automatically by the package for supported platforms).
    /// </summary>
    public sealed class FluxDB : IDisposable
    {
        private const string LibName = "flux";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr flux_init();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern IntPtr flux_query(IntPtr db, string sql);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr flux_result_get_text(IntPtr result);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void flux_free_result(IntPtr result);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void flux_close(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr flux_get_last_error();

        private IntPtr _handle;
        private bool _disposed;

        /// <summary>Open a new FluxDB instance.</summary>
        public FluxDB()
        {
            _handle = flux_init();
            if (_handle == IntPtr.Zero)
                throw new FluxException("Failed to initialize FluxDB: " + LastError());
        }

        /// <summary>Execute a query and return the result as a string.</summary>
        public string Query(string sql)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(FluxDB));
            if (sql == null) throw new ArgumentNullException(nameof(sql));

            IntPtr resultPtr = flux_query(_handle, sql);
            if (resultPtr == IntPtr.Zero)
                throw new FluxException("Query failed: " + LastError());

            IntPtr textPtr = flux_result_get_text(resultPtr);
            string output  = Marshal.PtrToStringAnsi(textPtr) ?? string.Empty;
            flux_free_result(resultPtr);
            return output;
        }

        /// <summary>Returns the last native error string.</summary>
        public static string LastError()
        {
            IntPtr p = flux_get_last_error();
            return p != IntPtr.Zero ? Marshal.PtrToStringAnsi(p) : "Unknown error";
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                if (_handle != IntPtr.Zero) { flux_close(_handle); _handle = IntPtr.Zero; }
                _disposed = true;
            }
            GC.SuppressFinalize(this);
        }

        ~FluxDB() => Dispose();
    }

    /// <summary>Exception thrown by FluxDB operations.</summary>
    public class FluxException : Exception
    {
        public FluxException(string message) : base(message) { }
    }
}
