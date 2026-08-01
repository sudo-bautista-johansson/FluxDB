// FluxDB.cs
using System;
using System.Runtime.InteropServices;

namespace Flux
{
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

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void flux_advance_tick(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern ulong flux_get_current_tick(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UIntPtr flux_get_delta_payload(IntPtr db, ulong last_ack_tick, IntPtr out_buffer, UIntPtr buffer_size);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern bool flux_run_script(IntPtr db, string lua_code);

        private IntPtr _handle;
        private bool _disposed;

        public FluxDB()
        {
            _handle = flux_init();
            if (_handle == IntPtr.Zero)
                throw new FluxException("Failed to initialize FluxDB: " + LastError());
        }

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

        public void AdvanceTick()
        {
            if (_disposed) throw new ObjectDisposedException(nameof(FluxDB));
            flux_advance_tick(_handle);
        }

        public ulong CurrentTick()
        {
            if (_disposed) throw new ObjectDisposedException(nameof(FluxDB));
            return flux_get_current_tick(_handle);
        }

        public byte[] GetDeltaPayload(ulong lastAckTick, int maxBytes = 65536)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(FluxDB));
            if (maxBytes <= 0) throw new ArgumentOutOfRangeException(nameof(maxBytes));

            byte[] buffer = new byte[maxBytes];
            IntPtr bufferPtr = Marshal.AllocHGlobal(buffer.Length);
            try
            {
                UIntPtr written = flux_get_delta_payload(_handle, lastAckTick, bufferPtr, (UIntPtr)buffer.Length);
                int count = checked((int)written.ToUInt64());
                if (count <= 0)
                    return Array.Empty<byte>();

                Marshal.Copy(bufferPtr, buffer, 0, count);
                Array.Resize(ref buffer, count);
                return buffer;
            }
            finally
            {
                Marshal.FreeHGlobal(bufferPtr);
            }
        }

        public bool RunScript(string luaCode)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(FluxDB));
            if (luaCode == null) throw new ArgumentNullException(nameof(luaCode));
            return flux_run_script(_handle, luaCode);
        }

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

    public class FluxException : Exception
    {
        public FluxException(string message) : base(message) { }
    }
}
