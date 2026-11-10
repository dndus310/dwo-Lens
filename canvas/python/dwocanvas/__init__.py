"""DwoCanvas tool-neutral Python side: schema validator, v1 -> v2 migration, naming rules, lint rules.

Standard library only. See canvas/python/README.md. Entry points:

    from dwocanvas.validate import validate, validate_document
    from dwocanvas.migrate import migrate
    from dwocanvas.naming import apply_naming, load_rules
    from dwocanvas.rules import check
    from dwocanvas.schema import Validator, load_schema, validate_schema

(The submodules ``migrate`` and ``validate`` share their main function's name, so the functions are not
re-exported at package level: ``dwocanvas.migrate`` / ``dwocanvas.validate`` stay the modules.)
"""

from . import migrate, naming, rules, schema, validate  # noqa: F401  (submodules)
from .schema import SCHEMA_V1, SCHEMA_V2  # noqa: F401

__version__ = "0.2.0"
