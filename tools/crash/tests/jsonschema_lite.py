# A small JSON Schema 2020-12 checker for the contract schemas.
#
# The `jsonschema` package is not installed system-wide on this machine, and
# the admin tool must not depend on it: this module covers exactly the
# keywords the four contract schemas use, and test_schema_checker.py proves it
# on the contract's own vectors (26 claims that must pass, 70 that must fail,
# each at a named JSON pointer).
#
# Differences with a full validator, all deliberate:
#   * only the keywords listed in KEYWORDS are understood; an unknown keyword
#     in a schema raises, so a contract change cannot pass unnoticed;
#   * patterns are ECMA-262 anchored expressions, translated the way
#     contract/tools/check_schemas.py does ($ -> \Z, because Python's $ also
#     matches before a final newline);
#   * errors are collected from every failing branch (anyOf, allOf, if/then),
#     which is what the vectors' invalid_at pointers name.
import json
import os
import re

KEYWORDS = frozenset({
    "$schema", "$id", "title", "description", "$defs", "$ref", "$comment",
    "type", "enum", "const", "properties", "required", "additionalProperties", "minProperties", "maxProperties",
    "items", "prefixItems", "minItems", "maxItems", "uniqueItems", "contains", "minContains", "maxContains",
    "pattern", "minLength", "maxLength", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum",
    "allOf", "anyOf", "oneOf", "not", "if", "then", "else",
})


class SchemaError(Exception):
    """The schema itself is not one this checker understands."""


def python_pattern(pattern):
    """Anchored ECMA-262 pattern as a Python one (see check_schemas.py)."""
    if pattern.endswith("$") and not pattern.endswith("\\$"):
        return pattern[:-1] + r"\Z"
    return pattern


def pointer(parts):
    return "".join("/" + str(p).replace("~", "~0").replace("/", "~1") for p in parts)


class Registry:
    """The contract schemas, addressed by file name (claim.v1.schema.json)."""

    def __init__(self, schema_dir):
        self.docs = {}
        for name in sorted(os.listdir(schema_dir)):
            if name.endswith(".schema.json"):
                with open(os.path.join(schema_dir, name), encoding="utf-8") as handle:
                    self.docs[name] = json.load(handle)
        if not self.docs:
            raise SchemaError(f"no *.schema.json in {schema_dir}")

    def resolve(self, ref, base):
        """(document name, node) of `ref`, relative to document `base`."""
        file_part, _, fragment = ref.partition("#")
        name = file_part or base
        if name not in self.docs:
            raise SchemaError(f"unknown schema document {name!r} in $ref {ref!r}")
        node = self.docs[name]
        for token in [t for t in fragment.split("/") if t != ""]:
            token = token.replace("~1", "/").replace("~0", "~")
            if isinstance(node, list):
                node = node[int(token)]
            else:
                node = node[token]
        return name, node

    def errors(self, ref, instance):
        """Validation errors of `instance` against `ref` (e.g. admin.v1.schema.json#/$defs/Stats)."""
        name, node = self.resolve(ref, base="")
        return _validate(self, name, node, instance, [])

    def check(self, ref, instance):
        found = self.errors(ref, instance)
        if found:
            raise AssertionError(f"{ref}: " + "; ".join(f"{p or '/'}: {m}" for p, m in found[:4]))


def _type_ok(kind, value):
    if kind == "object":
        return isinstance(value, dict)
    if kind == "array":
        return isinstance(value, list)
    if kind == "string":
        return isinstance(value, str)
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "null":
        return value is None
    if kind == "integer":
        if isinstance(value, bool):
            return False
        return isinstance(value, int) or (isinstance(value, float) and value.is_integer())
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    raise SchemaError(f"unknown type {kind!r}")


def _same(a, b):
    return json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)


def _validate(registry, base, schema, instance, path):
    if schema is True or schema == {}:
        return []
    if schema is False:
        return [(pointer(path), "no value is allowed here")]
    if not isinstance(schema, dict):
        raise SchemaError(f"schema must be an object or a boolean, got {type(schema).__name__}")
    unknown = set(schema) - KEYWORDS
    if unknown:
        raise SchemaError(f"unsupported keywords {sorted(unknown)}")
    errors = []
    here = pointer(path)

    if "$ref" in schema:
        ref_base, node = registry.resolve(schema["$ref"], base)
        errors += _validate(registry, ref_base, node, instance, path)

    if "type" in schema:
        kinds = schema["type"]
        kinds = [kinds] if isinstance(kinds, str) else kinds
        if not any(_type_ok(kind, instance) for kind in kinds):
            return errors + [(here, f"expected {' or '.join(kinds)}")]

    if "enum" in schema and not any(_same(instance, value) for value in schema["enum"]):
        errors.append((here, f"{instance!r} is not one of {schema['enum']}"))
    if "const" in schema and not _same(instance, schema["const"]):
        errors.append((here, f"{instance!r} is not {schema['const']!r}"))

    if isinstance(instance, str):
        if "pattern" in schema and not re.search(python_pattern(schema["pattern"]), instance):
            errors.append((here, f"does not match {schema['pattern']}"))
        if "minLength" in schema and len(instance) < schema["minLength"]:
            errors.append((here, f"shorter than {schema['minLength']}"))
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            errors.append((here, f"longer than {schema['maxLength']}"))

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append((here, f"below {schema['minimum']}"))
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append((here, f"above {schema['maximum']}"))
        if "exclusiveMinimum" in schema and instance <= schema["exclusiveMinimum"]:
            errors.append((here, f"not above {schema['exclusiveMinimum']}"))
        if "exclusiveMaximum" in schema and instance >= schema["exclusiveMaximum"]:
            errors.append((here, f"not below {schema['exclusiveMaximum']}"))

    if isinstance(instance, dict):
        errors += _validate_object(registry, base, schema, instance, path, here)
    if isinstance(instance, list):
        errors += _validate_array(registry, base, schema, instance, path, here)

    for keyword in ("allOf", "oneOf", "anyOf"):
        if keyword not in schema:
            continue
        branches = [_validate(registry, base, sub, instance, path) for sub in schema[keyword]]
        if keyword == "allOf":
            for branch in branches:
                errors += branch
        elif keyword == "anyOf" and not any(not branch for branch in branches):
            errors.append((here, "matches no branch of anyOf"))
            for branch in branches:
                errors += branch
        elif keyword == "oneOf" and sum(1 for branch in branches if not branch) != 1:
            errors.append((here, "must match exactly one branch of oneOf"))

    if "not" in schema and not _validate(registry, base, schema["not"], instance, path):
        errors.append((here, "matches a schema it must not match"))

    if "if" in schema:
        matched = not _validate(registry, base, schema["if"], instance, path)
        branch = schema.get("then") if matched else schema.get("else")
        if branch is not None:
            errors += _validate(registry, base, branch, instance, path)
    return errors


def _validate_object(registry, base, schema, instance, path, here):
    errors = []
    properties = schema.get("properties") or {}
    for name in schema.get("required", ()):
        if name not in instance:
            errors.append((here, f"missing property {name!r}"))
    if "minProperties" in schema and len(instance) < schema["minProperties"]:
        errors.append((here, f"fewer than {schema['minProperties']} properties"))
    if "maxProperties" in schema and len(instance) > schema["maxProperties"]:
        errors.append((here, f"more than {schema['maxProperties']} properties"))
    for name, value in instance.items():
        if name in properties:
            errors += _validate(registry, base, properties[name], value, path + [name])
        elif "additionalProperties" in schema:
            extra = schema["additionalProperties"]
            if extra is False:
                errors.append((here, f"unknown property {name!r}"))
            else:
                errors += _validate(registry, base, extra, value, path + [name])
    return errors


def _validate_array(registry, base, schema, instance, path, here):
    errors = []
    if "items" in schema:
        for index, value in enumerate(instance):
            errors += _validate(registry, base, schema["items"], value, path + [index])
    if "minItems" in schema and len(instance) < schema["minItems"]:
        errors.append((here, f"fewer than {schema['minItems']} items"))
    if "maxItems" in schema and len(instance) > schema["maxItems"]:
        errors.append((here, f"more than {schema['maxItems']} items"))
    if schema.get("uniqueItems"):
        seen = [json.dumps(v, sort_keys=True) for v in instance]
        if len(set(seen)) != len(seen):
            errors.append((here, "items are not unique"))
    if "contains" in schema:
        matches = sum(1 for value in instance if not _validate(registry, base, schema["contains"], value, path))
        low = schema.get("minContains", 1)
        high = schema.get("maxContains")
        if matches < low:
            errors.append((here, f"fewer than {low} items match contains"))
        if high is not None and matches > high:
            errors.append((here, f"more than {high} items match contains"))
    return errors
