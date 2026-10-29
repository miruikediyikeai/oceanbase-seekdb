#!/usr/bin/env python3
"""Exercise the SQL engine against a local mock, including malformed responses.

Run against an existing test instance:
  OBCLIENT=/path/to/obclient OB_PORT=10000 python3 test_ai_enhancement.py -v
Only ai_regression_* models/endpoints/tables in the test database are used.
"""

import json
import os
import socket
import subprocess
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def literal(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


def vector(text, dimension):
    return [len(text), sum(map(ord, text))] + [0.125] * (dimension - 2)


class MockHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append((self.path, body, dict(self.headers)))
        mode = self.server.mode
        if mode == "disconnect":
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        status = 200
        if self.path.endswith("/embeddings") or self.path.endswith("/api/embed"):
            inputs = body["input"]
            if isinstance(inputs, str):
                inputs = [inputs]
            dimension = body.get("dimensions", 4)
            items = [{"index": i, "embedding": vector(text, dimension)}
                     for i, text in enumerate(inputs)]
            if mode == "missing_index":
                items[0].pop("index")
            elif mode == "negative_index":
                items[0]["index"] = -1
            elif mode == "large_index":
                items[0]["index"] = len(items)
            elif mode == "float_index":
                items[0]["index"] = 0.5
            elif mode == "duplicate_index" and len(items) > 1:
                items[1]["index"] = items[0]["index"]
            elif mode == "missing_vector":
                items[0].pop("embedding")
            elif mode == "empty_vector":
                items[0]["embedding"] = []
            elif mode == "object_vector":
                items[0]["embedding"] = {"x": 1}
            elif mode == "string_element":
                items[0]["embedding"][0] = "1"
            elif mode == "null_element":
                items[0]["embedding"][0] = None
            elif mode == "bool_element":
                items[0]["embedding"][0] = True
            elif mode == "overflow_element":
                items[0]["embedding"][0] = 1e100
            elif mode == "wrong_dimension":
                items[0]["embedding"].append(1)
            elif mode == "missing_result":
                items.pop()
            elif mode == "inconsistent_dimensions" and len(items) > 1:
                items[1]["embedding"].append(1)
            if self.path.endswith("/api/embed"):
                response = {"embeddings": [item.get("embedding") for item in items]}
            else:
                response = {"data": list(reversed(items))}
            if mode == "object_data":
                response = {"data": {"embedding": [1, 2]}}
        elif self.path.endswith("/rerank"):
            items = [{"index": i, "relevance_score": int(text) % 5}
                     for i, text in enumerate(body["documents"])]
            if mode == "missing_result":
                items.pop()
            elif mode == "duplicate_index":
                items[-1]["index"] = items[0]["index"]
            elif mode == "negative_index":
                items[0]["index"] = -1
            elif mode == "large_index":
                items[0]["index"] = len(items)
            elif mode == "float_index":
                items[0]["index"] = 0.5
            elif mode == "missing_score":
                items[0].pop("relevance_score")
            elif mode == "string_score":
                items[0]["relevance_score"] = "bad"
            response = {"results": list(reversed(items))}
        else:
            content = body.get("prompt")
            if content is None:
                content = body["messages"][-1]["content"]
            if mode == "empty_completion":
                content = ""
            elif mode == "null_completion":
                content = None
            elif mode == "object_completion":
                content = {"text": "bad"}
            response = ({"response": content, "done": True}
                        if self.path.endswith("/api/generate")
                        else {"choices": [{"message": {"content": content}}]})
            if mode == "missing_completion":
                response = {"choices": []}
        if mode == "array_response":
            response = []
        if mode == "retry_once" and len(self.server.requests) == 1:
            response, status = {"error": "retry"}, 503
        if mode == "http_error":
            response, status = {"error": "bad request"}, 400
        if mode == "fail_second_batch" and len(self.server.requests) == 2:
            response, status = {"error": "batch failed"}, 400
        payload = b"{broken" if mode == "invalid_json" else json.dumps(response).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


class AIEnhancementTest(unittest.TestCase):
    @classmethod
    def sql(cls, query, error=False):
        cmd = [os.environ.get("OBCLIENT", "obclient"), "--no-defaults", "-h",
               os.environ.get("OB_HOST", "127.0.0.1"), "-P", os.environ.get("OB_PORT", "10000"),
               "-u", os.environ.get("OB_USER", "root"), "--default-character-set=utf8mb4",
               "--batch", "--skip-column-names", "--raw", "-D", os.environ.get("OB_DATABASE", "test")]
        if os.environ.get("OB_PASSWORD"):
            cmd.append("--password=" + os.environ["OB_PASSWORD"])
        proc = subprocess.run(cmd, input="set ob_query_timeout=30000000;\n" + query + ";\n",
                              text=True, encoding="utf-8", capture_output=True, timeout=45)
        if error:
            if proc.returncode == 0:
                raise AssertionError("SQL unexpectedly succeeded: " + query[:200])
            if "ERROR " not in proc.stderr:
                raise AssertionError("Client failed without a SQL error: " + proc.stderr)
        elif proc.returncode:
            raise AssertionError(proc.stderr + "\nSQL: " + query[:500])
        return proc.stdout.strip()

    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
        cls.server.mode = "ok"
        cls.server.requests = []
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.models = []
        cls.addClassCleanup(cls.cleanup)
        for suffix, kind, provider, path in [
                ("c", "completion", "openai", "/v1/chat/completions"),
                ("e", "dense_embedding", "openai", "/v1/embeddings"),
                ("r", "rerank", "siliconflow", "/v1/rerank"),
                ("oc", "completion", "oLlAmA", "/api/generate"),
                ("oe", "dense_embedding", "OLLAMA", "/api/embed")]:
            name = "ai_regression_" + suffix
            # Use unique names per process to coexist with other local test runs.
            name += "_" + str(os.getpid())
            setattr(cls, suffix, name)
            config = json.dumps({"type": kind, "model_name": "mock-model"})
            cls.sql("call DBMS_AI_SERVICE.CREATE_AI_MODEL(" + literal(name) + "," + literal(config) + ")")
            cls.models.append(name)
            config = {"ai_model_name": name, "scope": "all", "url": "http://127.0.0.1:"
                      + str(cls.server.server_port) + path, "provider": provider}
            if provider.lower() != "ollama":
                config["access_key"] = "mock-key"
            cls.sql("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(" + literal(name + "_ep")
                    + "," + literal(json.dumps(config)) + ")")
        cls.table = "ai_regression_rows_" + str(os.getpid())
        cls.sql("create table " + cls.table + " (id int primary key, content longtext, dim int, model varchar(128))")
        rows = ["(%d,%s,4,%s)" % (i, "null" if i % 7 == 0 else literal(str(i)), literal(cls.e))
                for i in range(1, 81)]
        cls.sql("insert into " + cls.table + " values " + ",".join(rows))

    @classmethod
    def cleanup(cls):
        try:
            if hasattr(cls, "table"):
                cls.sql("drop table if exists " + cls.table)
            for name in reversed(cls.models):
                cls.sql("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(" + literal(name + "_ep") + ")")
                cls.sql("call DBMS_AI_SERVICE.DROP_AI_MODEL(" + literal(name) + ")")
        finally:
            cls.server.shutdown()
            cls.server.server_close()
            cls.thread.join()

    def setUp(self):
        self.server.mode = "ok"
        self.server.requests.clear()

    def complete(self, prompt, config="null", model=None):
        result = self.sql("select json_quote(ai_complete(" + literal(model or self.c) + "," + prompt + "," + config + "))")
        return json.loads(result)

    def test_prompt_json_nested_repeat_unicode_escape(self):
        prompt = "ai_prompt('{0}/{0}/{1}/{2}',json_array(1,true),json_object('key','值'),ai_prompt('内层:{0}','\\\"\\\\\\n'))"
        text = self.complete(prompt)
        self.assertTrue(text.startswith('[1, true]/[1, true]/{"key": "值"}/内层:'))
        self.assertNotIn('"template"', text)
        self.assertEqual(self.server.requests[-1][1]["messages"][-1]["content"], text)

    def test_prompt_replacement_does_not_reparse_arguments(self):
        self.assertEqual(self.complete("ai_prompt('{0}-{0}','{1}')"), "{1}-{1}")

    def test_prompt_invalid_indices(self):
        for template in ("{1}", "{999999999999999999999999999999999999}"):
            with self.subTest(template=template):
                self.sql("select ai_complete(" + literal(self.c) + ",ai_prompt(" + literal(template) + ",'x'))", error=True)
        self.assertEqual(self.server.requests, [])

    def test_prompt_longtext(self):
        prompt = "ai_prompt(concat(cast(repeat('你',30000) as char),'{0}{0}'),'世界')"
        text = self.complete(prompt)
        self.assertEqual(text, "你" * 30000 + "世界世界")
        table = self.table + "_prompt"
        self.sql("create table " + table + "(t longtext,a text)")
        try:
            self.sql("insert into " + table + " values(concat(repeat('你',30000),'{0}/{0}'),repeat('好',10000))")
            value = self.sql("select json_quote(ai_complete(" + literal(self.c) + ",ai_prompt(t,a))) from " + table)
            self.assertEqual(json.loads(value), "你" * 30000 + "好" * 10000 + "/" + "好" * 10000)
            value = self.sql("select json_quote(ai_complete(" + literal(self.c) + ",t)) from " + table)
            self.assertEqual(json.loads(value), "你" * 30000 + "{0}/{0}")
        finally:
            self.sql("drop table " + table)

    def test_completion_config_and_null(self):
        self.assertEqual(self.complete("'hello'"), "hello")
        self.assertEqual(self.complete("'hello'", "json_object('temperature',0.2,'max_tokens',16)"), "hello")
        body = self.server.requests[-1][1]
        self.assertEqual(body["max_tokens"], 16)
        self.assertEqual(body["temperature"], 0.2)
        for config in ("json_array(1)", "'[]'", "'broken'"):
            self.sql("select ai_complete(" + literal(self.c) + ",'hello'," + config + ")", error=True)

    def test_completion_bad_responses(self):
        for mode in ("empty_completion", "null_completion", "object_completion", "missing_completion",
                     "array_response", "invalid_json", "http_error", "disconnect"):
            with self.subTest(mode=mode):
                self.server.mode = mode
                self.sql("select ai_complete(" + literal(self.c) + ",'hello')", error=True)
        self.server.mode = "ok"
        self.assertEqual(self.complete("'healthy'"), "healthy")

    def test_http_retry(self):
        self.server.mode = "retry_once"
        self.assertEqual(self.complete("'retried'"), "retried")
        self.assertEqual(len(self.server.requests), 2)

    def test_ollama_native_protocol_and_endpoint_alter(self):
        self.assertEqual(self.complete("'你好'", "json_object('temperature',0.3,'max_tokens',9)", self.oc), "你好")
        path, body, headers = self.server.requests[-1]
        self.assertEqual(path, "/api/generate")
        self.assertIs(body["stream"], False)
        self.assertEqual(body["options"], {"temperature": 0.3, "num_predict": 9})
        self.assertNotIn("Authorization", headers)
        self.assertEqual(headers["Content-Type"], "application/json")
        config = literal(json.dumps({"provider": "Ollama", "access_key": ""}))
        self.sql("call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(" + literal(self.oc + "_ep") + "," + config + ")")
        self.assertEqual(self.complete("'after alter'", model=self.oc), "after alter")
        result = self.sql("select ai_embed(" + literal(self.oe) + ",'abc')")
        self.assertEqual(json.loads(result), vector("abc", 4))
        self.assertEqual(self.server.requests[-1][0], "/api/embed")

    def test_embed_single_and_dimensions(self):
        for dimension in (None, 2, 8):
            args = "" if dimension is None else "," + str(dimension)
            value = self.sql("select ai_embed(" + literal(self.e) + ",'abc'" + args + ")")
            self.assertEqual(json.loads(value), vector("abc", dimension or 4))
        for dimension in ("null", "0", "-1", "1.2", "'4'"):
            self.sql("select ai_embed(" + literal(self.e) + ",'abc'," + dimension + ")", error=True)
        before = len(self.server.requests)
        self.assertEqual(self.sql("select ai_embed(" + literal(self.e) + ",null)"), "NULL")
        self.assertEqual(len(self.server.requests), before)

    def test_embed_longtext(self):
        table = self.table + "_lob"
        self.sql("create table " + table + "(id int primary key,content longtext)")
        try:
            self.sql("insert into " + table + " values(1,repeat('你',10000)),(2,null),(3,repeat('好',12000))")
            values = self.sql("select id,ai_embed(" + literal(self.e) + ",content) from " + table + " order by id").splitlines()
            self.assertEqual(json.loads(values[0].split("\t", 1)[1]), vector("你" * 10000, 4))
            self.assertEqual(values[1], "2\tNULL")
            self.assertEqual(json.loads(values[2].split("\t", 1)[1]), vector("好" * 12000, 4))
        finally:
            self.sql("drop table " + table)

    def test_embed_bad_responses(self):
        modes = ("missing_index", "negative_index", "large_index", "float_index", "missing_vector",
                 "empty_vector", "object_vector", "string_element", "null_element", "bool_element",
                 "overflow_element", "wrong_dimension", "missing_result", "object_data",
                 "array_response", "invalid_json", "http_error")
        for mode in modes:
            with self.subTest(mode=mode):
                self.server.mode = mode
                self.sql("select ai_embed(" + literal(self.e) + ",'abc',4)", error=True)

    def assert_rows(self, sql, ids):
        rows = self.sql(sql).splitlines()
        self.assertEqual(len(rows), len(ids))
        for row, expected_id in zip(rows, ids):
            key, value = row.split("\t", 1)
            self.assertEqual(int(key), expected_id)
            if expected_id % 7 == 0:
                self.assertEqual(value, "NULL")
            else:
                self.assertEqual(json.loads(value), vector(str(expected_id), 4))

    def test_embed_batch_null_filter_limit_and_multiple_batches(self):
        query = "select id,ai_embed(" + literal(self.e) + ",content) from " + self.table
        self.assert_rows(query + " order by id", list(range(1, 81)))
        self.assertTrue(any(len(body["input"]) > 1 for _, body, _ in self.server.requests),
                        "table scans must use a multi-input HTTP request")
        self.server.requests.clear()
        self.assert_rows(query + " where id % 2 = 0 order by id limit 11", list(range(2, 23, 2)))
        self.server.requests.clear()
        self.assertEqual(self.sql(query + " where id < 0"), "")
        self.assertEqual(self.sql(query + " limit 0"), "")
        self.assertEqual(self.server.requests, [])
        self.assert_rows(query + " where id % 7 = 0 order by id", list(range(7, 81, 7)))
        self.assertEqual(self.server.requests, [])
        self.server.requests.clear()
        # A small engine batch forces repeated requests and row index resets.
        self.assert_rows(query.replace("select id", "select /*+ opt_param('rowsets_max_rows' 8) */ id")
                         + " order by id", list(range(1, 81)))
        self.assertGreater(len(self.server.requests), 1)

    def test_embed_batch_bad_response_and_recovery(self):
        query = "select id,ai_embed(" + literal(self.e) + ",content) from " + self.table + " order by id"
        for mode in ("duplicate_index", "inconsistent_dimensions", "missing_result", "http_error"):
            with self.subTest(mode=mode):
                self.server.mode = mode
                self.sql(query, error=True)
        self.server.mode = "ok"
        self.assert_rows(query, list(range(1, 81)))
        self.server.requests.clear()
        self.server.mode = "fail_second_batch"
        self.sql(query.replace("select id", "select /*+ opt_param('rowsets_max_rows' 8) */ id"), error=True)
        self.assertEqual(len(self.server.requests), 2)
        self.server.mode = "ok"
        self.assert_rows(query, list(range(1, 81)))

    def test_embed_varying_model_and_dimension(self):
        query = "select id,ai_embed(model,content,dim) from " + self.table + " order by id"
        self.assert_rows(query, list(range(1, 81)))
        rows = self.sql("select id,ai_embed(model,content,2+mod(id,4)) from "
                        + self.table + " order by id").splitlines()
        for i, row in enumerate(rows, 1):
            key, value = row.split("\t", 1)
            self.assertEqual(int(key), i)
            if i % 7 == 0:
                self.assertEqual(value, "NULL")
            else:
                self.assertEqual(json.loads(value), vector(str(i), 2+i % 4))
        model = "case when mod(id,2)=0 then " + literal(self.e) + " else " + literal(self.oe) + " end"
        self.assert_rows("select id,ai_embed(" + model + ",content,4) from " + self.table + " order by id",
                         list(range(1, 81)))

    def test_rerank_global_order_and_original_ids(self):
        docs = [str(i) for i in range(47)]
        expected = sorted(range(len(docs)), key=lambda i: (-(i % 5), i))
        result = self.sql("select ai_rerank(" + literal(self.r) + ",'q'," + literal(json.dumps(docs)) + ",null)")
        self.assertEqual([x["index"] for x in json.loads(result)], expected)
        self.assertEqual([len(body["documents"]) for _, body, _ in self.server.requests], [20, 20, 7])
        self.server.requests.clear()
        objects = [{"id": "original-" + str(i), "body": str(i), "extra": i} for i in range(47)]
        result = self.sql("select ai_rerank(" + literal(self.r) + ",'q'," + literal(json.dumps(objects)) + ",'body')")
        values = json.loads(result)
        self.assertEqual([x["id"] for x in values], ["original-" + str(i) for i in expected])
        self.assertEqual([x["extra"] for x in values], expected)
        self.assertEqual([len(body["documents"]) for _, body, _ in self.server.requests], [20, 20, 7])

    def test_rerank_bad_responses_and_inputs(self):
        for mode in ("missing_result", "duplicate_index", "negative_index", "large_index", "float_index",
                     "missing_score", "string_score", "array_response", "invalid_json", "http_error"):
            with self.subTest(mode=mode):
                self.server.mode = mode
                self.sql("select ai_rerank(" + literal(self.r) + ",'q','[\"1\",\"2\"]')", error=True)
        self.server.mode = "ok"
        for docs, key in (("[]", "null"), ('["x",{}]', "null"), ('[{"body":1}]', "'body'"),
                          ('[{"text":"x"}]', "'body'")):
            self.sql("select ai_rerank(" + literal(self.r) + ",'q'," + literal(docs) + "," + key + ")", error=True)
        self.assertEqual(self.sql("select 1+2"), "3")


if __name__ == "__main__":
    unittest.main()
