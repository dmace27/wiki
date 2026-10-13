#!/usr/bin/env python3
"""Verify task 7 through the real CLI with a deterministic local model server.

Uses only Python's standard library. All projects, retained notes, and model
responses are disposable. No real Ollama service or private corpus is used.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class ModelHandler(BaseHTTPRequestHandler):
    """Return exactly cited contributions from the request's selected page."""

    def log_message(self, format_string, *args):
        # Keep test stdout a concise success/failure report.
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        prompt = request["messages"][-1]["content"]
        target = json.loads(prompt.split("Target:\n", 1)[1].split("\n\nEvidence pages:", 1)[0])
        page = json.loads(prompt.split("Evidence pages:\n", 1)[1].split("\n\nCitation offsets", 1)[0])[0]
        assert self.path == "/api/chat"
        assert request["stream"] is False
        citation = {
            "page_id": page["page_id"],
            "start_char": 0,
            "end_char": len(page["text"].encode("utf-8")),
            "quote": page["text"].strip(),
        }
        semester = "A" if "SemesterA" in page["text"] else "B"
        article = {"title": target["concept"], "slug": target["slug"], "aliases": []}
        if "article_id" in target:
            article["article_id"] = target["article_id"]
        proposal = {
            "schema_version": 1,
            "operation": target["operation"],
            "article": article,
            "sections": [{
                "key": "working_explanation",
                "heading": "Explanation",
                "blocks": [{"kind": "paragraph", "text": f"Semester{semester} approved explanation.",
                            "citations": [citation]}],
            }],
            "related_concepts": [{"title": f"Semester {semester} Relationship",
                                  "reason": f"Semester{semester} supported relationship.",
                                  "citations": [citation]}],
        }
        if semester == "A":
            proposal["sections"].append({
                "key": "example", "heading": "Earlier example",
                "blocks": [{"kind": "paragraph", "text": "SemesterA useful example.",
                            "citations": [citation]}],
            })
        response = json.dumps({"done": True, "message": {"content": json.dumps(proposal)}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


def run_smoke(executable: Path, project: Path, base_url: str):
    """Use real command routing, curl, JSON review, approval, apply, and FTS."""
    def invoke(*arguments, expected_status=0, human=False):
        command = [str(executable), "--project", str(project)]
        if not human:
            command.append("--json")
        result = subprocess.run(command + list(arguments), capture_output=True, text=True, timeout=30)
        assert result.returncode == expected_status, (command, result.returncode, result.stdout, result.stderr)
        if human:
            return result.stdout
        document = json.loads(result.stdout)
        assert document["ok"] == (expected_status == 0), document
        return document.get("result", document.get("error"))

    invoke("init")
    config_path = project / "kc.json"
    config = json.loads(config_path.read_text())
    config["providers"]["llm"]["ollama"]["base_url"] = base_url
    config_path.write_text(json.dumps(config))

    def import_notes(semester):
        path = project / f"semester-{semester}.txt"
        path.write_text(f"Markov Chains: Semester{semester} exact supporting evidence.\n")
        result = invoke("import", str(path))
        # Imports report ordered per-file outcomes; retain the explicit ID.
        source_id = result["files"][0]["result"]["source_id"]
        invoke("extract", source_id)
        return source_id

    def compile_proposal(source_id, regenerate=False):
        flags = ["--regenerate"] if regenerate else []
        result = invoke("compile", "--concept", "Markov Chains", "--source", source_id, *flags)
        assert result["selected_page_count"] == 1, result
        return result["proposal_id"]

    def approve(proposal_id):
        preview = invoke("proposal", "preview", proposal_id)["preview"]
        assert preview["valid"] and preview["after"]
        invoke("proposal", "approve", proposal_id, "--preview", preview["preview_sha256"])
        return preview

    a = import_notes("A")
    initial = compile_proposal(a)
    approve(initial)
    applied = invoke("apply", initial)
    article = project / applied["vault_path"]
    old_content = article.read_bytes()
    user_notes = b"\n## User notes\nKeep my surrounding text exactly.\n"
    article.write_bytes(old_content + user_notes)

    b = import_notes("B")
    additive = compile_proposal(b)
    review = invoke("proposal", "show", additive)
    changes = review["update_changes"]
    assert changes["mode"] == "additive" and changes["removed_claims"] == []
    assert changes["retained_claim_count"] == 3
    assert len({item["source_version_id"] for item in review["citation_evidence"]}) == 2
    human = invoke("proposal", "show", additive, human=True)
    assert "Update mode: additive" in human and "Removed claims" in human
    approve(additive)
    assert article.read_bytes() == old_content + user_notes
    invoke("apply", additive)
    content = article.read_bytes()
    assert content.endswith(user_notes)
    assert b"SemesterA useful example" in content and b"SemesterB approved explanation" in content
    assert "Semester A Relationship" in content.decode()
    assert invoke("search", "SemesterA")["articles"]
    assert invoke("search", "SemesterB")["articles"]

    # Identical B contributions neither add duplicate prose nor change bytes.
    duplicate = compile_proposal(b)
    review = invoke("proposal", "show", duplicate)
    assert review["update_changes"]["added_claims"] == []
    approve(duplicate)
    invoke("apply", duplicate)
    assert article.read_bytes() == content

    regenerate = compile_proposal(b, regenerate=True)
    review = invoke("proposal", "show", regenerate)
    assert review["update_changes"]["mode"] == "regenerate"
    assert len(review["update_changes"]["removed_claims"]) == 3
    assert "SemesterA useful example." in json.dumps(review["update_changes"]["removed_claims"])
    human = invoke("proposal", "show", regenerate, human=True)
    assert "Update mode: regenerate" in human and "SemesterA useful example." in human
    approve(regenerate)
    assert article.read_bytes() == content

    # An edit after review/approval blocks apply without replacing any bytes.
    edited = content.replace(b"SemesterA approved explanation", b"Manual managed change")
    article.write_bytes(edited)
    error = invoke("apply", regenerate, expected_status=5)
    assert "managed article content changed" in error["message"]
    assert article.read_bytes() == edited
    article.write_bytes(content)
    fresh = compile_proposal(b, regenerate=True)
    approve(fresh)
    invoke("apply", fresh)
    assert article.read_bytes().endswith(user_notes)
    assert b"SemesterA" not in article.read_bytes()
    assert invoke("search", "SemesterA")["articles"] == []
    assert invoke("search", "SemesterB")["articles"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kc", required=True, type=Path, help="Built CLI executable")
    arguments = parser.parse_args()
    executable = arguments.kc.resolve(strict=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="kc-task7-") as directory:
            run_smoke(executable, Path(directory) / "project", f"http://127.0.0.1:{server.server_port}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("Task 7 CLI smoke passed: accumulation, exact citations, review, duplicate merging, stale approval, regeneration, user notes, and search.")


if __name__ == "__main__":
    main()
