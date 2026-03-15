let editor;

// Initialize the UI and fetch initial data
document.addEventListener('DOMContentLoaded', () => {
    fetchStats();
    
    // Setup tabs
    document.querySelectorAll('.tab[data-target]').forEach(tab => {
        tab.addEventListener('click', (e) => {
            document.querySelectorAll('.tab[data-target]').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.results-content > div:not(pre), .results-content pre').forEach(v => {
                v.classList.remove('view-active');
                v.classList.add('view-hidden');
            });
            
            const targetId = e.target.getAttribute('data-target');
            e.target.classList.add('active');
            
            const targetView = document.getElementById(`${targetId}-view`) || document.getElementById(`console-${targetId}`) || document.getElementById(targetId);
            if (targetView) {
                targetView.classList.remove('view-hidden');
                targetView.classList.add('view-active');
            }
        });
    });

    // Initialize CodeMirror IDE
    const textArea = document.getElementById('code-editor');
    
    // Custom SQL dialect for VeldraDB (GQL)
    CodeMirror.defineMIME("text/x-gql", {
        name: "sql",
        keywords: CodeMirror.resolveMode("sql").keywords,
        builtin: {"FIND": true, "NEAR": true, "WITHIN": true, "SPAWN": true, "PREFAB": true, "SNAPSHOT": true, "RESTORE": true, "TICKS": true, "SINCE": true},
        atoms: {"false": true, "true": true, "null": true},
        operatorChars: /^[*+\-%<>!=]/,
        dateSQL: {},
        support: {}
    });

    editor = CodeMirror.fromTextArea(textArea, {
        mode: "text/x-gql",
        theme: "dracula",
        lineNumbers: true,
        matchBrackets: true,
        autoCloseBrackets: true,
        indentUnit: 4,
        extraKeys: {"Ctrl-Space": "autocomplete"}
    });

    // Auto-trigger IntelliSense on typing
    editor.on("keyup", function (cm, event) {
        if (!cm.state.completionActive && /*Enables keyboard navigation in autocomplete list*/
            event.keyCode >= 65 && event.keyCode <= 90) { // Only trigger on letters
            CodeMirror.commands.autocomplete(cm, null, {completeSingle: false});
        }
    });

    // Force size to fit flex container
    editor.setSize("100%", "100%");
});

function fetchStats() {
    fetch('/api/stats')
        .then(response => response.json())
        .then(data => {
            document.getElementById('db-name-label').textContent = data.database;
            document.getElementById('stat-entities').textContent = data.entity_count.toLocaleString();
            document.getElementById('stat-archetypes').textContent = data.archetypes;
            document.getElementById('stat-pages').textContent = data.active_pages;
        })
        .catch(err => {
            console.error('Failed to fetch stats:', err);
            document.getElementById('connection-status').textContent = 'Disconnected';
            document.querySelector('.status-indicator').classList.remove('online');
            document.getElementById('db-name-label').textContent = 'Disconnected';
        });
}

function executeQuery() {
    const query = editor.getValue();
    const outputView = document.getElementById('console-output');
    
    if (!query.trim()) return;

    outputView.textContent += `\n> Executing: ${query.split('\n')[0].substring(0, 30)}...\n`;
    
    document.querySelector('.tab[data-target="output"]').click();

    fetch('/api/query', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ query: query })
    })
    .then(response => response.json())
    .then(data => {
        outputView.textContent += data.message + '\n';
        outputView.scrollTop = outputView.scrollHeight;
        fetchStats();
    })
    .catch(err => {
        outputView.textContent += `Error: ${err.message}\n`;
        outputView.scrollTop = outputView.scrollHeight;
    });
}

function formatQuery() {
    let code = editor.getValue();
    const keywords = ['select', 'from', 'where', 'and', 'or', 'insert', 'into', 'values', 'update', 'set', 'delete', 'find', 'near', 'within', 'spawn', 'prefab', 'with'];
    
    keywords.forEach(kw => {
        const regex = new RegExp(`\\b${kw}\\b`, 'gi');
        code = code.replace(regex, kw.toUpperCase());
    });
    
    editor.setValue(code);
}

function loadQuery(id) {
    if (id === 1) {
        editor.setValue("FIND entities\nNEAR (10.0, 0.0, 5.0)\nWITHIN 50.0\nWHERE tag = 'enemy';");
    } else if (id === 2) {
        editor.setValue("SPAWN PREFAB 'goblin'\nWITH health = 100, tag = 'enemy';");
    } else if (id === 3) {
        editor.setValue("DELETE FROM entities\nWHERE health <= 0;");
    }
}
