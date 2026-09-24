"""Refresh the authored NVL72 kit from the checked-in GB200_Connections.blend.
Run: blender -b --python scripts/build_connections.py [-- --no-render]
This is a source-based rebuild, not a recreation of proprietary rack meshes from scratch.
The source blend and manifest must be present. No external Modular_Kit.blend is needed.
"""
from pathlib import Path
import runpy
import sys

ROOT = Path(__file__).resolve().parents[1]
for source in [ROOT / "GB200_Connections.blend", ROOT / "reports/connection_manifest.json"]:
    if not source.is_file():
        raise FileNotFoundError(f"Required checked-in authoring source missing: {source}")
sys.path.insert(0, str(ROOT / "scripts"))
# Both migrations are idempotent and share rear_data_layout.LAYOUT_U with the bake/thermal model.
runpy.run_path(str(ROOT / "scripts/migrate_nvl72_layout.py"), run_name="__main__")
runpy.run_path(str(ROOT / "scripts/migrate_rear_data.py"), run_name="__main__")
runpy.run_path(str(ROOT / "scripts/validate_ports.py"), run_name="__main__")
print("BUILD_CONNECTIONS_PASS: checked-in source, 37-device NVL72 layout")
