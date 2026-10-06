import importlib.util
import json
import os
import pathlib
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
VISIBILITY = HERE / "tool-visibility.py"
HARNESS = pathlib.Path(os.environ.get("DECIDER_EVAL", HERE / "decider-eval.py"))
EFFECTS = HERE.parents[1] / "src/feature/llm/services/turn/tool-effects.json"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


visibility = load("tool_visibility", VISIBILITY)
harness = load("decider_eval", HARNESS)

ROLE_ACCESS = """
inline constexpr RoleMask kBaselineBit = static_cast<RoleMask>(1U << kRoleBitCount);
inline constexpr std::string_view kSurveillanceModule = "surveillance";
inline constexpr RoleMask kEveryRole =
    roleBits({UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest});
inline constexpr RoleMask kEveryRoleBaseline = static_cast<RoleMask>(kEveryRole | kBaselineBit);
inline constexpr RoleMask kOwnerOnly = roleBit(UserRole::Owner);
"""
MODULE_SNAPSHOT = 'inline constexpr std::string_view kCoreModule = "core";\n'
CAPABILITY = """
inline constexpr RoleMask kOwnerResident = roleBits({UserRole::Owner, UserRole::Resident});
inline constexpr std::array kCapabilities = std::to_array<CapabilitySpec>({
    {.id = "notes.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "notes.write", .module = kCoreModule, .roles = kOwnerResident},
    {.id = "vault.open", .module = kSurveillanceModule, .roles = kOwnerOnly},
});
"""


def spec(name, capability, module='"core"', annotations="{}"):
    return (f'server->add(spec({{.name = "{name}", .title = "", .annotations = {annotations}, '
            f'.module = {module}, .capability = "{capability}"}}), handler);\n')


class ParserTest(unittest.TestCase):
    def build(self, tools):
        root = pathlib.Path(tempfile.mkdtemp())
        files = {"packages/lib/auth/src/auth/role-access.hxx": ROLE_ACCESS,
                 "packages/lib/auth/src/auth/module-snapshot.hxx": MODULE_SNAPSHOT,
                 "packages/lib/auth/src/auth/capability.hxx": CAPABILITY}
        files.update({source: "" for source in visibility.TOOL_SOURCES})
        files[visibility.TOOL_SOURCES[0]] = tools
        for relative, text in files.items():
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_text(text)
        return root

    def test_a_tool_is_visible_to_exactly_the_roles_its_capability_grants(self):
        root = self.build(spec("note.show", "notes.read", annotations="{.readOnly = true}")
                          + spec("note.edit", "notes.write") + spec("vault.unlock", "vault.open", '"surveillance"'))
        document, problems = visibility.parse(root)
        self.assertEqual(problems, [])
        self.assertEqual(document["roles"]["owner"], ["note.edit", "note.show", "vault.unlock"])
        self.assertEqual(document["roles"]["resident"], ["note.edit", "note.show"])
        self.assertEqual(document["roles"]["guard"], ["note.show"])
        self.assertEqual(document["roles"]["guest"], ["note.show"])
        self.assertEqual(document["tools"]["vault.unlock"], {"capability": "vault.open", "module": "surveillance"})

    def test_a_declared_module_that_is_not_the_capabilitys_is_a_problem(self):
        _, problems = visibility.parse(self.build(spec("note.show", "notes.read", '"surveillance"')))
        self.assertEqual(len(problems), 1)
        self.assertIn("resolves to module 'surveillance'", problems[0])

    def test_a_helper_that_assigns_the_module_wins_over_the_literal(self):
        helper = ("constexpr const char* kNotes = \"core\";\n"
                  "argus::mcp::ToolSpec wrap(argus::mcp::ToolSpec base)\n{\n  base.module = kNotes;\n  return base;\n}\n")
        root = self.build(helper + spec("note.show", "notes.read", '""').replace("spec(", "wrap("))
        document, problems = visibility.parse(root)
        self.assertEqual(problems, [])
        self.assertEqual(document["tools"]["note.show"]["module"], "core")

    def test_a_tool_whose_resolved_module_is_empty_is_a_problem(self):
        _, problems = visibility.parse(self.build(spec("note.show", "notes.read", '""')))
        self.assertEqual(len(problems), 1)
        self.assertIn("resolves to module ''", problems[0])

    def test_a_helper_does_not_touch_a_tool_it_does_not_wrap(self):
        helper = ("constexpr const char* kNotes = \"surveillance\";\n"
                  "argus::mcp::ToolSpec wrap(argus::mcp::ToolSpec base)\n{\n  base.module = kNotes;\n  return base;\n}\n")
        _, problems = visibility.parse(self.build(helper + spec("note.show", "notes.read", '"core"')))
        self.assertEqual(problems, [])

    def test_a_capability_the_table_does_not_declare_is_a_problem(self):
        _, problems = visibility.parse(self.build(spec("note.show", "notes.nothing")))
        self.assertEqual(len(problems), 1)
        self.assertIn("kCapabilities does not declare", problems[0])

    def test_a_name_without_a_capability_is_not_a_tool_and_one_declared_twice_must_agree(self):
        mention = 'call = AppAction{.name = "note.show", .arguments = a};\n'
        document, problems = visibility.parse(self.build(mention + spec("note.show", "notes.read")))
        self.assertEqual(problems, [])
        self.assertEqual(list(document["tools"]), ["note.show"])
        with self.assertRaises(ValueError):
            visibility.parse(self.build(spec("note.show", "notes.read") + spec("note.show", "notes.write")))

    def test_an_annotated_read_only_tool_is_reported_by_the_spec_reader(self):
        specs = visibility.tool_specs(self.build(spec("note.show", "notes.read", annotations="{.readOnly = true}")
                                                 + spec("note.edit", "notes.write", annotations="{.idempotent = true}")))
        self.assertTrue(specs["note.show"]["readOnly"])
        self.assertFalse(specs["note.edit"]["readOnly"])

    def test_a_role_expression_the_parser_cannot_read_fails_loudly(self):
        with self.assertRaises(ValueError):
            visibility.evaluate("kSomethingElse", {})


class BackendTest(unittest.TestCase):
    def test_the_committed_visibility_is_what_the_backend_sources_declare(self):
        document, problems = visibility.parse()
        self.assertEqual(problems, [])
        committed = json.loads((HERE / "tool-visibility.json").read_text())
        self.assertEqual(committed, document,
                         "tool-visibility.json is stale: python3 -I services/llm/tests/eval/tool-visibility.py "
                         "--write services/llm/tests/eval/tool-visibility.json")

    def test_the_decider_vocabulary_is_exactly_the_tools_the_backend_declares(self):
        committed = json.loads((HERE / "tool-visibility.json").read_text())
        self.assertEqual(set(harness.FAMILY_OF), set(committed["tools"]))

    def test_the_harness_reads_the_read_only_list_the_turn_flow_compiles(self):
        effects = set(json.loads(EFFECTS.read_text())["readOnly"])
        self.assertEqual(harness.READ_TOOLS, effects)
        specs = visibility.tool_specs(visibility.ROOT)
        self.assertLessEqual(effects, set(specs))
        annotated = {name for name, spec in specs.items() if spec["readOnly"]}
        self.assertLessEqual(annotated, effects, "a tool annotated read-only would be guarded as a write")

    def test_a_role_is_offered_only_the_tools_it_holds(self):
        owner, resident = set(harness.offered("owner")), set(harness.offered("resident"))
        guard, guest = set(harness.offered("guard")), set(harness.offered("guest"))
        self.assertNotIn("modules.request", owner)
        self.assertIn("modules.enable", owner)
        self.assertIn("modules.request", resident)
        self.assertNotIn("modules.enable", resident)
        self.assertIn("calendar.create_event", resident)
        for role in (guard, guest):
            self.assertIn("modules.request", role)
            self.assertNotIn("calendar.create_event", role)
            self.assertNotIn("memory.remember", role)
            self.assertNotIn("app.set_guard_mode", role)
        self.assertIn("app.show_camera", guard)

    def test_a_role_with_no_declared_visibility_is_refused(self):
        with self.assertRaises(SystemExit):
            harness.offered("stranger")


if __name__ == "__main__":
    unittest.main()
