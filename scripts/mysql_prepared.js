// scripts/mysql_prepared.js
//
// MySQL prepared-statement workload for wrkx (ADR 0005, P6-5).
// QuickJS equivalent of mysql_prepared.lua.
// Use with:
//   ./wrkx -t4 -c100 -d10s -R500 -s scripts/mysql_prepared.js \
//          mysql://wrkx:secret@localhost/wrkx

const stmt = mysql.prepare("SELECT ?");
let counter = 0;

function request() {
    counter++;
    return mysql.execute(stmt, String(counter % 1000));
}
