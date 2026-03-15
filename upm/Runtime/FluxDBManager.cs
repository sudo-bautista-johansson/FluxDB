// FluxDBManager.cs
// Part of the FluxDB Unity Package (com.flux.fluxdb)
using UnityEngine;

namespace Flux.Unity
{
    /// <summary>
    /// A simple MonoBehaviour wrapper to manage the lifecyle of the FluxDB native instance.
    /// Add this to an empty GameObject in your initial scene.
    /// </summary>
    [DefaultExecutionOrder(-100)]
    public class FluxDBManager : MonoBehaviour
    {
        private FluxDB _db;

        /// <summary>Global access to the database instance.</summary>
        public static FluxDBManager Instance { get; private set; }

        void Awake()
        {
            if (Instance != null && Instance != this)
            {
                Destroy(gameObject);
                return;
            }

            Instance = this;
            DontDestroyOnLoad(gameObject);

            try
            {
                _db = new FluxDB();
                Debug.Log("[FluxDB] Native Engine Initialized.");
            }
            catch (System.Exception e)
            {
                Debug.LogError($"[FluxDB] Initialization failed: {e.Message}");
            }
        }

        /// <summary>
        /// Execute a FluxDB query.
        /// </summary>
        /// <param name="sql">The query string</param>
        /// <returns>Query result as text (usually JSON) or an error string</returns>
        public string Query(string sql)
        {
            if (_db == null)
            {
                return "[FluxDB] Error: Database is not initialized.";
            }

            try
            {
                return _db.Query(sql);
            }
            catch (System.Exception e)
            {
                return $"[FluxDB] Query Error: {e.Message}";
            }
        }

        void OnDestroy()
        {
            if (_db != null)
            {
                _db.Dispose();
                _db = null;
                Debug.Log("[FluxDB] Native Engine Shut Down.");
            }
        }
    }
}
