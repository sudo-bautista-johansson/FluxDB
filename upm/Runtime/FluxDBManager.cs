// FluxDBManager.cs
using UnityEngine;

namespace Flux.Unity
{
    [DefaultExecutionOrder(-100)]
    public class FluxDBManager : MonoBehaviour
    {
        private FluxDB _db;
        public static FluxDBManager Instance { get; private set; }

        void Awake()
        {
            if (Instance != null && Instance != this) { Destroy(gameObject); return; }
            Instance = this;
            DontDestroyOnLoad(gameObject);
            try { _db = new FluxDB(); Debug.Log("[FluxDB] Native Engine Initialized."); }
            catch (System.Exception e) { Debug.LogError($"[FluxDB] Initialization failed: {e.Message}"); }
        }

        public string Query(string sql)
        {
            if (_db == null) return "[FluxDB] Error: Database is not initialized.";
            try { return _db.Query(sql); }
            catch (System.Exception e) { return $"[FluxDB] Query Error: {e.Message}"; }
        }

        public void AdvanceTick()
        {
            if (_db == null) { Debug.Log("[FluxDB] Error: Database is not initialized."); return; }
            try { _db.AdvanceTick(); }
            catch (System.Exception e) { Debug.LogError($"[FluxDB] AdvanceTick Error: {e.Message}"); }
        }

        public ulong CurrentTick()
        {
            if (_db == null) { Debug.Log("[FluxDB] Error: Database is not initialized."); return 0; }
            try { return _db.CurrentTick(); }
            catch (System.Exception e) { Debug.LogError($"[FluxDB] CurrentTick Error: {e.Message}"); return 0; }
        }

        public byte[] GetDeltaPayload(ulong lastAckTick, int maxBytes = 65536)
        {
            if (_db == null) { Debug.Log("[FluxDB] Error: Database is not initialized."); return System.Array.Empty<byte>(); }
            try { return _db.GetDeltaPayload(lastAckTick, maxBytes); }
            catch (System.Exception e) { Debug.LogError($"[FluxDB] GetDeltaPayload Error: {e.Message}"); return System.Array.Empty<byte>(); }
        }

        public bool RunScript(string luaCode)
        {
            if (_db == null) { Debug.Log("[FluxDB] Error: Database is not initialized."); return false; }
            try { return _db.RunScript(luaCode); }
            catch (System.Exception e) { Debug.LogError($"[FluxDB] RunScript Error: {e.Message}"); return false; }
        }

        void OnDestroy() { if (_db != null) { _db.Dispose(); _db = null; Debug.Log("[FluxDB] Native Engine Shut Down."); } }
    }
}
