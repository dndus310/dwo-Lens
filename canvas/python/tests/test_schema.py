import unittest

import support  # noqa: F401  (sys.path)
from dwocanvas.schema import (SCHEMA_V1, SCHEMA_V2, Validator, json_equal, load_schema, pointer_join,
                              resolve_pointer, schema_dir, validate_schema)


def errors(schema, instance):
    return Validator(schema).errors(instance)


def keywords(schema, instance):
    return [e.keyword for e in errors(schema, instance)]


class TypeKeywords(unittest.TestCase):
    def test_basic_types(self):
        self.assertEqual(keywords({"type": "string"}, "a"), [])
        self.assertEqual(keywords({"type": "string"}, 1), ["type"])
        self.assertEqual(keywords({"type": "object"}, {}), [])
        self.assertEqual(keywords({"type": "array"}, {}), ["type"])
        self.assertEqual(keywords({"type": "null"}, None), [])

    def test_number_excludes_bool(self):
        self.assertEqual(keywords({"type": "number"}, 1.5), [])
        self.assertEqual(keywords({"type": "number"}, True), ["type"])
        self.assertEqual(keywords({"type": "boolean"}, True), [])
        self.assertEqual(keywords({"type": "boolean"}, 0), ["type"])

    def test_integer(self):
        self.assertEqual(keywords({"type": "integer"}, 3), [])
        self.assertEqual(keywords({"type": "integer"}, 3.0), [])
        self.assertEqual(keywords({"type": "integer"}, 3.5), ["type"])
        self.assertEqual(keywords({"type": "integer"}, False), ["type"])

    def test_type_list(self):
        schema = {"type": ["boolean", "string"]}
        self.assertEqual(keywords(schema, "x"), [])
        self.assertEqual(keywords(schema, False), [])
        self.assertEqual(keywords(schema, 2), ["type"])


class ValueKeywords(unittest.TestCase):
    def test_enum_and_const(self):
        self.assertEqual(keywords({"enum": ["a", 1]}, "a"), [])
        self.assertEqual(keywords({"enum": ["a", 1]}, True), ["enum"])  # True is not 1 in JSON
        self.assertEqual(keywords({"const": "dwocanvas/2"}, "dwocanvas/2"), [])
        self.assertEqual(keywords({"const": "dwocanvas/2"}, "designbridge/1"), ["const"])

    def test_numeric_bounds(self):
        schema = {"type": "number", "minimum": 0, "maximum": 1}
        self.assertEqual(keywords(schema, 0), [])
        self.assertEqual(keywords(schema, 1), [])
        self.assertEqual(keywords(schema, -0.1), ["minimum"])
        self.assertEqual(keywords(schema, 1.1), ["maximum"])

    def test_string_keywords(self):
        self.assertEqual(keywords({"minLength": 1}, ""), ["minLength"])
        self.assertEqual(keywords({"minLength": 1}, "a"), [])
        schema = {"pattern": "^[A-Za-z_][A-Za-z0-9_]*$"}
        self.assertEqual(keywords(schema, "Apply_1"), [])
        self.assertEqual(keywords(schema, "1Apply"), ["pattern"])
        self.assertEqual(keywords({"pattern": "b"}, "abc"), [])  # unanchored search

    def test_json_equal(self):
        self.assertTrue(json_equal(1, 1.0))
        self.assertFalse(json_equal(1, True))
        self.assertTrue(json_equal({"a": [1, 2]}, {"a": [1, 2]}))
        self.assertFalse(json_equal({"a": [1, 2]}, {"a": [2, 1]}))


class ObjectKeywords(unittest.TestCase):
    def test_required_and_properties(self):
        schema = {"type": "object", "required": ["id"], "properties": {"id": {"type": "string"}}}
        self.assertEqual(keywords(schema, {"id": "x"}), [])
        self.assertEqual(keywords(schema, {}), ["required"])
        errs = errors(schema, {"id": 3})
        self.assertEqual([(e.keyword, e.path) for e in errs], [("type", "/id")])

    def test_additional_properties_false_and_pattern(self):
        schema = {"properties": {"a": {}}, "patternProperties": {"^x-": {}}, "additionalProperties": False}
        self.assertEqual(keywords(schema, {"a": 1, "x-figma": {}}), [])
        errs = errors(schema, {"b": 1})
        self.assertEqual([(e.keyword, e.path) for e in errs], [("additionalProperties", "/b")])

    def test_additional_properties_schema(self):
        schema = {"additionalProperties": {"type": "integer"}}
        self.assertEqual(keywords(schema, {"a": 1, "b": 2}), [])
        self.assertEqual([e.path for e in errors(schema, {"a": 1, "b": "2"})], ["/b"])

    def test_pattern_properties_validate_values(self):
        schema = {"patternProperties": {"^n_": {"type": "number"}}}
        self.assertEqual([e.path for e in errors(schema, {"n_a": "x", "other": "x"})], ["/n_a"])


class ArrayKeywords(unittest.TestCase):
    def test_items_and_bounds(self):
        schema = {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 4}
        self.assertEqual(keywords(schema, [1, 2, 3]), [])
        self.assertEqual(keywords(schema, [1, 2]), ["minItems"])
        self.assertEqual(keywords(schema, [1, 2, 3, 4, 5]), ["maxItems"])
        self.assertEqual([e.path for e in errors(schema, [1, "x", 3])], ["/1"])

    def test_unique_items(self):
        schema = {"uniqueItems": True}
        self.assertEqual(keywords(schema, ["a", "b"]), [])
        errs = errors(schema, ["a", "b", "a"])
        self.assertEqual([(e.keyword, e.path) for e in errs], [("uniqueItems", "/2")])
        self.assertEqual(keywords(schema, [1, True]), [])


class Composition(unittest.TestCase):
    def test_ref_and_defs(self):
        schema = {"$defs": {"pos": {"type": "number", "minimum": 0}}, "properties": {"w": {"$ref": "#/$defs/pos"}}}
        self.assertEqual(keywords(schema, {"w": 1}), [])
        self.assertEqual(keywords(schema, {"w": -1}), ["minimum"])

    def test_ref_to_root(self):
        schema = {"properties": {"children": {"type": "array", "items": {"$ref": "#"}}, "id": {"type": "string"}}}
        errs = errors(schema, {"children": [{"children": [{"id": 5}]}]})
        self.assertEqual([e.path for e in errs], ["/children/0/children/0/id"])

    def test_any_of(self):
        schema = {"anyOf": [{"enum": ["button", "list"]}, {"type": "string", "pattern": "^x-[a-z]+$"}]}
        self.assertEqual(keywords(schema, "button"), [])
        self.assertEqual(keywords(schema, "x-slot"), [])
        self.assertEqual(keywords(schema, "Button"), ["anyOf"])

    def test_boolean_schemas(self):
        self.assertEqual(keywords(True, 1), [])
        self.assertEqual(keywords(False, 1), ["false"])

    def test_pointer_escaping(self):
        self.assertEqual(pointer_join("", "a/b~c"), "/a~1b~0c")
        self.assertEqual(resolve_pointer({"a/b~c": [0, 7]}, "/a~1b~0c/1"), 7)
        errs = errors({"properties": {"a/b": {"type": "string"}}}, {"a/b": 1})
        self.assertEqual(errs[0].path, "/a~1b")


class SchemaFiles(unittest.TestCase):
    def test_load_both_versions(self):
        self.assertEqual(load_schema(SCHEMA_V2)["$id"], SCHEMA_V2)
        self.assertEqual(load_schema(SCHEMA_V1)["$id"], SCHEMA_V1)

    def test_override_directory(self):
        self.assertEqual(load_schema(SCHEMA_V2, schema_dir())["$id"], SCHEMA_V2)
        with self.assertRaises(OSError):
            load_schema(SCHEMA_V2, support.REPO_ROOT / "does-not-exist")

    def test_v2_minimal_document(self):
        self.assertEqual(validate_schema(support.v2_doc()), [])

    def test_v2_node_errors_have_paths(self):
        doc = support.v2_doc(support.node("1:1", "Screen", role="Button", widgetName="1bad", box={"x": 0, "y": 0, "w": -1, "h": 1}))
        paths = sorted(e.path for e in validate_schema(doc))
        self.assertEqual(paths, ["/documents/0/root/box/w", "/documents/0/root/role", "/documents/0/root/widgetName"])

    def test_v2_rejects_unknown_top_level_but_allows_x(self):
        doc = support.v2_doc()
        doc["images"] = {}
        doc["x-figma"] = {"page": "1"}
        self.assertEqual([e.path for e in validate_schema(doc)], ["/images"])

    def test_v1_samples_are_valid_v1(self):
        for name in ("sample_shop.json", "DwoMockup.dwocanvas.json"):
            with self.subTest(name=name):
                self.assertEqual(validate_schema(support.load_json(support.SAMPLES_DIR / name), SCHEMA_V1), [])


if __name__ == "__main__":
    unittest.main()
