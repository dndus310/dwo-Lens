import unittest

import support
from support import node, v2_doc
from dwocanvas import rules
from dwocanvas.validate import validate


def found(doc, rule=None):
    items = rules.check(doc)
    return [(i["level"], i["rule"], i["node"]) for i in items if rule is None or i["rule"] == rule]


def instance(id_, ref, name="Entry"):
    return node(id_, name, "instance", component={"ref": ref, "name": name})


ROW = {"id": "K:1", "name": "Row", "kind": "component", "root": node("K:1", "Row", "component")}


class Clean(unittest.TestCase):
    def test_minimal_document_has_no_findings(self):
        self.assertEqual(found(v2_doc()), [])

    def test_item_shape(self):
        doc = v2_doc(capabilities=[])
        item = rules.check(doc)[0]
        self.assertEqual(set(item), {"level", "node", "name", "message", "rule"})


class Ids(unittest.TestCase):
    def test_missing_id(self):
        root = node("1", "Screen", children=[{"name": "NoId", "type": "frame"}, node("", "Empty")])
        self.assertEqual([x[:2] for x in found(v2_doc(root), rules.NODE_ID_MISSING)], [("error", "node-id-missing")] * 2)

    def test_duplicate_id_within_document(self):
        root = node("1", "Screen", children=[node("2", "A"), node("2", "B")])
        self.assertEqual(found(v2_doc(root), rules.NODE_ID_DUPLICATE), [("error", "node-id-duplicate", "2")])

    def test_same_id_in_different_documents_is_fine(self):
        doc = v2_doc(node("1", "Screen", children=[node("2", "A")]),
                     components=[{"id": "K", "name": "Comp", "root": node("2", "Comp", "component")}])
        self.assertEqual(found(doc, rules.NODE_ID_DUPLICATE), [])

    def test_duplicate_document_id(self):
        doc = v2_doc(components=[{"id": "D:1", "name": "Comp", "root": node("K", "Comp", "component")}])
        self.assertEqual([x[1] for x in found(doc, rules.DOCUMENT_ID_DUPLICATE)], ["document-id-duplicate"])


class WidgetNames(unittest.TestCase):
    def test_duplicate_effective_names(self):
        root = node("1", "Screen", children=[node("2", "btn_Apply"), node("3", "Apply", widgetName="Apply")])
        self.assertEqual(found(v2_doc(root), rules.WIDGET_NAME_DUPLICATE), [("warning", "widget-name-duplicate", "3")])

    def test_non_variable_duplicates_are_info(self):
        root = node("1", "Screen", children=[node("2", "Label"), node("3", "Label")])
        self.assertEqual(found(v2_doc(root), rules.WIDGET_NAME_DUPLICATE), [("info", "widget-name-duplicate", "3")])

    def test_cousins_and_ignored_and_list_entries(self):
        root = node("1", "Screen", children=[
            node("2", "A", children=[node("3", "$Label")]),
            node("4", "B", children=[node("5", "$Label")]),
            node("6", "_Guide"), node("7", "_Guide"),
            node("8", "list_Items", children=[instance("9", "K:1"), instance("10", "K:1")]),
        ])
        self.assertEqual(found(v2_doc(root, components=[ROW]), rules.WIDGET_NAME_DUPLICATE), [])


class Assets(unittest.TestCase):
    def test_unresolved_assets(self):
        root = node("1", "Screen", fills=[{"type": "image", "image": "img_x"}], bake={"image": "img_bake"},
                    states={"hovered": {"fills": [{"type": "image", "image": "img_hover"}]}},
                    children=[node("2", "Img", fills=[{"type": "image"}])])
        doc = v2_doc(root, preview="img_preview", assets={"img_ok": {"data": "AAAA"}})
        got = found(doc, rules.ASSET_MISSING)
        self.assertEqual(sorted(got), sorted([("error", "asset-missing", ""), ("error", "asset-missing", "1"),
                                              ("error", "asset-missing", "1"), ("error", "asset-missing", "1"),
                                              ("warning", "asset-missing", "2")]))

    def test_resolved_assets(self):
        root = node("1", "Screen", fills=[{"type": "image", "image": "a"}], bake={"image": "a"})
        self.assertEqual(found(v2_doc(root, preview="a", assets={"a": {"uri": "a.png"}}), rules.ASSET_MISSING), [])


class Components(unittest.TestCase):
    def test_unresolved_ref(self):
        root = node("1", "Screen", children=[instance("2", "K:404"), instance("3", "K:1")])
        self.assertEqual(found(v2_doc(root, components=[ROW]), rules.COMPONENT_MISSING), [("warning", "component-missing", "2")])

    def test_button_instances_are_exempt(self):
        root = node("1", "Screen", children=[node("2", "btn_Ok", "instance", component={"ref": "K:404"})])
        self.assertEqual(found(v2_doc(root), rules.COMPONENT_MISSING), [])

    def test_list_first_child(self):
        root = node("1", "Screen", children=[
            node("2", "list_Good", children=[instance("3", "K:1")]),
            node("4", "list_Bad", children=[node("5", "Row")]),
            node("6", "Tiles", role="tile", children=[]),
            node("7", "Plain", children=[node("8", "Row")]),
        ])
        self.assertEqual(found(v2_doc(root, components=[ROW]), rules.LIST_ENTRY),
                         [("warning", "list-entry", "4"), ("warning", "list-entry", "6")])


class Capabilities(unittest.TestCase):
    def test_mismatches(self):
        root = node("1", "Screen",
                    fills=[{"type": "linear", "stops": []}, {"type": "image", "image": "a"}],
                    effects=[{"type": "drop-shadow"}], confidence=0.5, inferred=["box"])
        doc = v2_doc(root, assets={"a": {"data": ""}}, capabilities=["stable-ids"])
        msgs = [i["message"] for i in rules.check(doc) if i["rule"] == rules.CAPABILITY]
        self.assertEqual(len(msgs), 4)
        for cap in ("paint.gradient", "paint.image", "effects", "inferred"):
            self.assertTrue(any(cap in m for m in msgs), cap)
        self.assertTrue(all(i["level"] == "warning" for i in rules.check(doc) if i["rule"] == rules.CAPABILITY))

    def test_state_gradient_counts(self):
        root = node("1", "btn_X", states={"hovered": {"fills": [{"type": "radial"}]}})
        self.assertEqual([x[0] for x in found(v2_doc(root, capabilities=["stable-ids"]), rules.CAPABILITY)], ["warning"])

    def test_undeclared(self):
        doc = v2_doc()
        del doc["capabilities"]
        self.assertEqual(found(doc), [("info", "capabilities-undeclared", ""), ("info", "stable-ids", "")])

    def test_draft_and_inferred(self):
        draft = v2_doc(source={"tool": "inference", "mode": "draft"}, capabilities=["stable-ids"])
        self.assertEqual(found(draft, rules.DRAFT_NOT_INFERRED), [("error", "draft-without-inferred", "")])
        ok = v2_doc(source={"tool": "inference", "mode": "draft"}, capabilities=["inferred"])
        self.assertEqual(found(ok, rules.DRAFT_NOT_INFERRED) + found(ok, rules.INFERRED_NOT_DRAFT), [])
        extract = v2_doc(capabilities=["inferred", "stable-ids"])
        self.assertEqual(found(extract, rules.INFERRED_NOT_DRAFT), [("warning", "inferred-without-draft", "")])

    def test_extension_role(self):
        root = node("1", "Screen", children=[node("2", "Slot", role="x-inventorySlot")])
        self.assertEqual(found(v2_doc(root), rules.EXTENSION_ROLE), [("info", "extension-role", "2")])

    def test_stable_ids(self):
        self.assertEqual(found(v2_doc(capabilities=["layout.flow"]), rules.STABLE_IDS), [("info", "stable-ids", "")])
        self.assertEqual(found(v2_doc(), rules.STABLE_IDS), [])


class Combined(unittest.TestCase):
    def test_schema_errors_point_at_nodes(self):
        root = node("1", "Screen", children=[node("2", "Bad", role="Banana")])
        issues = validate(v2_doc(root))
        schema = [i for i in issues if i["rule"] == "schema"]
        self.assertEqual(len(schema), 1)
        self.assertEqual((schema[0]["level"], schema[0]["node"], schema[0]["path"]), ("error", "2", "/documents/0/root/children/0/role"))

    def test_not_an_object(self):
        self.assertEqual([i["rule"] for i in validate([1, 2])], ["schema"])

    def test_unknown_schema(self):
        doc = v2_doc()
        doc["schema"] = "other/9"
        levels = {(i["level"], i["rule"]) for i in validate(doc)}
        self.assertIn(("error", "schema"), levels)


if __name__ == "__main__":
    unittest.main()
