#!/usr/bin/env python3
"""Task 8 CLI regression with disposable notes and a local model fixture.

The PNG and OCR status are deterministic fixture data; this does not measure
real OCR or model quality. The native viewer launch has an independent injected
argv regression in C++ so automated tests do not start the user's GUI apps.
"""

import argparse
import base64
import hashlib
import json
import shlex
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading
import sys
from http.server import ThreadingHTTPServer

sys.dont_write_bytecode = True
from accumulated_updates_smoke import ModelHandler


def verify(executable, project, base_url):
    def invoke(*arguments, expected_status=0, human=False):
        command = [str(executable), "--project", str(project)]
        if not human:
            command.append("--json")
        response = subprocess.run(command + list(arguments), capture_output=True, text=True, timeout=30)
        assert response.returncode == expected_status, (arguments, response.returncode, response.stdout, response.stderr)
        if human:
            return response.stdout
        envelope = json.loads(response.stdout)
        assert envelope["ok"] == (expected_status == 0), envelope
        return envelope

    def result(*arguments):
        return invoke(*arguments)["result"]

    def compile_source(source_id, regenerate=False):
        flags = ["--regenerate"] if regenerate else []
        return result("compile", "--concept", "Markov Chains", "--source", source_id, *flags)["proposal_id"]

    def approve(proposal_id, preview, overwrite=False):
        flags = ["--allow-overwrite-user-file"] if overwrite else []
        result("proposal", "approve", proposal_id, "--preview", preview["preview_sha256"], *flags)

    def import_source(semester):
        path = project / f"semester-{semester}.txt"
        path.write_text(f"Markov Chains: Semester{semester} exact supporting evidence.\n")
        source_id = result("import", str(path))["sources"][0]["source_id"]
        result("extract", source_id)
        return source_id

    result("init")
    config_file = project / "kc.json"
    config = json.loads(config_file.read_text())
    config["providers"]["llm"]["ollama"]["base_url"] = base_url
    config_file.write_text(json.dumps(config))
    source_a = import_source("A")
    state_path = project / config["paths"]["state"]
    with sqlite3.connect(state_path) as database:
        old_page, old_text = database.execute("SELECT page_id,text FROM source_pages").fetchone()
        image = project / config["paths"]["cache"] / "pages" / "image with spaces.png"
        image.parent.mkdir(parents=True, exist_ok=True)
        # Valid, redistributable one-pixel PNG generated from a standard fixture.
        image.write_bytes(base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
        database.execute("UPDATE source_pages SET text_status='ocr_unreviewed',image_path=?,image_sha256=?",
                         (str(image.relative_to(project)), hashlib.sha256(image.read_bytes()).hexdigest()))

    inspection = result("review", "extraction", source_a, "--page", "1")
    page = inspection["pages"][0]
    assert page["image_absolute_path"] == str(image)
    assert page["text"] == old_text and page["text_status"] == "ocr_unreviewed"
    assert shlex.split(page["open_image_command"]) == ["kc", "--project", str(project), "review", "extraction", source_a, "--page", "1", "--open-image"]
    accepted = result("review", "extraction", source_a, "--page", "1", "--accept")
    assert accepted["pages"][0]["text"] == old_text
    assert accepted["pages"][0]["text_status"] == "reviewed"
    assert accepted["corrected"]["page_id"] == old_page

    rejected = compile_source(source_a)
    result("proposal", "reject", rejected, "--reason", "Found an extraction error")
    corrected = result("review", "extraction", source_a, "--page", "1", "--text",
                       "Markov Chains: SemesterA corrected evidence.\n")
    new_page = corrected["corrected"]["page_id"]
    assert new_page != old_page and corrected["corrected"]["previous_page_id"] == old_page
    history = result("proposal", "show", rejected)
    assert {item["citation"]["page_id"] for item in history["citation_evidence"]} == {old_page}
    assert history["citation_evidence"][0]["extracted_text"] == old_text
    assert result("review", "extraction", source_a)["pages"][0]["page_id"] == new_page

    create = compile_source(source_a)
    article = project / config["paths"]["vault"] / "Markov Chains.md"
    user_collision = "# My existing article\nPreserve this until I approve a replacement.\n"
    article.write_text(user_collision)
    blocked = invoke("proposal", "preview", create, expected_status=5)["result"]["preview"]
    assert not blocked["valid"] and blocked["untracked_collision"]
    assert blocked["before"] == user_collision and blocked["after"]
    assert "-Preserve this" in blocked["diff"]
    consent = result("proposal", "show", create, "--allow-overwrite-user-file")["preview"]
    assert consent["valid"] and consent["untracked_collision"]
    invoke("proposal", "approve", create, "--preview", consent["preview_sha256"], expected_status=5)
    approve(create, consent, overwrite=True)
    assert article.read_text() == user_collision
    # An unused approval can be rejected without a vault write.
    result("proposal", "reject", create, "--reason", "Reconsider replacement")
    assert article.read_text() == user_collision
    create = compile_source(source_a)
    consent = result("proposal", "preview", create, "--allow-overwrite-user-file")["preview"]
    approve(create, consent, overwrite=True)
    applied = result("apply", create, "--allow-overwrite-user-file")
    assert article.read_text() == consent["after"]
    assert (project / applied["backup_path"]).read_text() == user_collision

    user_notes = "\n## My notes\nProtected surrounding bytes.\n"
    article.write_text(article.read_text() + user_notes)
    source_b = import_source("B")
    update = compile_source(source_b, regenerate=True)
    review = result("proposal", "show", update)
    preview = review["preview"]
    assert preview["valid"] and preview["after"].endswith(user_notes)
    assert review["update_changes"]["removed_claims"]
    assert "-SemesterA useful example" in preview["diff"]
    assert "+SemesterB approved explanation" in preview["diff"]
    human = invoke("proposal", "preview", update, human=True)
    assert "Destination:" in human and "Exact proposed Markdown" in human and "Rendered diff" in human
    assert "--preview " + preview["preview_sha256"] in human
    before = article.read_text()
    approve(update, preview)
    result("proposal", "revoke", update)
    assert article.read_text() == before
    approve(update, preview)
    newer = before + "\nA user addition after approval.\n"
    article.write_text(newer)
    invoke("apply", update, expected_status=5)
    assert article.read_text() == newer
    fresh = result("proposal", "preview", update)["preview"]
    assert fresh["valid"] and fresh["matches_approval"] is False
    result("proposal", "revoke", update)
    invoke("proposal", "approve", update, "--preview", preview["preview_sha256"], expected_status=5)
    approve(update, fresh)
    result("apply", update)
    assert article.read_text() == fresh["after"]
    assert article.read_text().endswith(user_notes + "\nA user addition after approval.\n")
    invoke("proposal", "revoke", update, expected_status=3)
    invoke("proposal", "reject", update, expected_status=3)
    assert result("proposal", "show", rejected)["citation_evidence"][0]["extracted_text"] == old_text

    # Even invalid stored state yields a structured preview diagnostic. Neither
    # inspection nor failed approval copies sources or adds an apply/backup.
    malformed = compile_source(source_b)
    with sqlite3.connect(state_path) as database:
        database.execute("UPDATE proposals SET payload_json='{}' WHERE proposal_id=?", (malformed,))
    invalid = invoke("proposal", "preview", malformed, expected_status=5)["result"]["preview"]
    assert not invalid["valid"] and invalid["issues"]
    assert article.read_text() == fresh["after"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kc", required=True, type=Path)
    args = parser.parse_args()
    executable = args.kc.resolve(strict=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="kc-task8-") as directory:
            verify(executable, Path(directory) / "project with spaces and 'quotes'", f"http://127.0.0.1:{server.server_port}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("Task 8 CLI smoke passed: inspect/accept/correct OCR, retained citation history, exact preview/apply, collisions, deletion diff, revoke/reject approval, stale preview, and validation diagnostics.")


if __name__ == "__main__":
    main()
