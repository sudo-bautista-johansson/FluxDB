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

        void OnDestroy() { if (_db != null) { _db.Dispose(); _db = null; Debug.Log("[FluxDB] Native Engine Shut Down."); } }
    }
}
