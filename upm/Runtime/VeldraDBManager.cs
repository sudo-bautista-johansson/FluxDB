// VeldraDBManager.cs
// Part of the VeldraDB Unity Package (com.veldra.veldradb)
using UnityEngine;

namespace Veldra.Unity
{
    /// <summary>
    /// A simple MonoBehaviour wrapper to manage the lifecyle of the VeldraDB native instance.
    /// Add this to an empty GameObject in your initial scene.
    /// </summary>
    [DefaultExecutionOrder(-100)]
    public class VeldraDBManager : MonoBehaviour
    {
        private VeldraDB _db;

        /// <summary>Global access to the database instance.</summary>
        public static VeldraDBManager Instance { get; private set; }

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
                _db = new VeldraDB();
                Debug.Log("[VeldraDB] Native Engine Initialized.");
            }
            catch (System.Exception e)
            {
                Debug.LogError($"[VeldraDB] Initialization failed: {e.Message}");
            }
        }

        /// <summary>
        /// Execute a VeldraDB query.
        /// </summary>
        /// <param name="sql">The query string</param>
        /// <returns>Query result as text (usually JSON) or an error string</returns>
        public string Query(string sql)
        {
            if (_db == null)
            {
                return "[VeldraDB] Error: Database is not initialized.";
            }

            try
            {
                return _db.Query(sql);
            }
            catch (System.Exception e)
            {
                return $"[VeldraDB] Query Error: {e.Message}";
            }
        }

        void OnDestroy()
        {
            if (_db != null)
            {
                _db.Dispose();
                _db = null;
                Debug.Log("[VeldraDB] Native Engine Shut Down.");
            }
        }
    }
}
