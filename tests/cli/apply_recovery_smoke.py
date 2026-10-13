"""Task 9 CLI history/restore regression with disposable deterministic evidence.

Crash and real filesystem failure tests live in C++ so they can terminate at
exact durability boundaries. This smoke verifies the supported CLI/JSON flow
through the actual model HTTP boundary without real model quality claims.
"""
import argparse
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

sys.dont_write_bytecode = True
from accumulated_updates_smoke import ModelHandler


def verify(executable, project, base_url):
    def invoke(*args, status=0):
        response = subprocess.run([str(executable), "--project", str(project), "--json", *args],
                                  capture_output=True, text=True, timeout=30)
        assert response.returncode == status, (args, response.stdout, response.stderr)
        document = json.loads(response.stdout)
        assert document["ok"] == (status == 0), document
        return document.get("result", document)

    invoke("init")
    config_path = project / "kc.json"
    config = json.loads(config_path.read_text())
    config["providers"]["llm"]["ollama"]["base_url"] = base_url
    config_path.write_text(json.dumps(config))

    def apply(semester):
        notes = project / f"semester-{semester}.txt"
        notes.write_text(f"Markov Chains: Semester{semester} exact supporting evidence.\n")
        source = invoke("import", str(notes))["sources"][0]["source_id"]
        invoke("extract", source)
        flags = ["--regenerate"] if semester != "A" else []
        proposal = invoke("compile", "--concept", "Markov Chains", "--source", source, *flags)["proposal_id"]
        preview = invoke("proposal", "preview", proposal)["preview"]
        invoke("proposal", "approve", proposal, "--preview", preview["preview_sha256"])
        result = invoke("apply", proposal)
        return result, preview["after"]

    first, first_bytes = apply("A")
    second, second_bytes = apply("B")
    article = project / first["vault_path"]
    assert article.read_text() == second_bytes
    history = invoke("history", first["article_id"])
    assert len(history["revisions"]) == 2
    assert history["revisions"][1]["current"] is True
    assert history["revisions"][0]["restorable"] is True

    inspection = invoke("restore", first["apply_run_id"])
    assert inspection["after"] == first_bytes
    assert "SemesterB" in inspection["diff"] and "SemesterA" in inspection["diff"]
    assert article.read_text() == second_bytes
    user_bytes = second_bytes + "\nMy newer user notes.\n"
    article.write_text(user_bytes)
    invoke("restore", first["apply_run_id"], "--preview", inspection["preview_sha256"], status=5)
    assert article.read_text() == user_bytes
    fresh = invoke("restore", first["apply_run_id"])
    assert "-My newer user notes." in fresh["diff"]
    restored = invoke("restore", first["apply_run_id"], "--preview", fresh["preview_sha256"])
    assert restored["restore_run_id"].startswith("run_")
    assert article.read_text() == first_bytes
    history = invoke("history", first["article_id"])
    assert history["revisions"][0]["current"] is True
    assert history["revisions"][1]["current"] is False
    assert history["restores"][0]["from_apply_run_id"] == second["apply_run_id"]
    assert (project / history["restores"][0]["backup_path"]).read_text() == user_bytes
    assert invoke("search", "SemesterA")["articles"]
    assert not invoke("search", "SemesterB")["articles"]
    with sqlite3.connect(project / config["paths"]["state"]) as db:
        assert db.execute("SELECT current_apply_run_id FROM articles").fetchone()[0] == first["apply_run_id"]
        assert db.execute("SELECT COUNT(*) FROM pragma_foreign_key_check").fetchone()[0] == 0
        restored_pages = {row[0] for row in db.execute("SELECT page_id FROM article_citations")}
        first_proposal = history["revisions"][0]["proposal_id"]
        approved_pages = {row[0] for row in db.execute("SELECT page_id FROM proposal_citations WHERE proposal_id=?", (first_proposal,))}
        assert restored_pages == approved_pages
    recovered = invoke("recover")
    assert recovered["recovered"] and not recovered["inventory"]["retained_artifacts"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kc", required=True, type=Path)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="kc-task9-") as directory:
            verify(args.kc.resolve(strict=True), Path(directory) / "project with spaces",
                   f"http://127.0.0.1:{server.server_port}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("Task 9 CLI smoke passed: history, exact restore preview, stale token, user edit review, citations, audit, FTS, and recovery inventory.")


if __name__ == "__main__":
    main()
