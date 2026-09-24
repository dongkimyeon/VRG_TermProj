"""Resave L_Test through UE serialization; inspect references without binary patching."""
import unreal, json
from pathlib import Path
world=unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test')
wp=unreal.WorldPartitionBlueprintLibrary
wp.load_actors([d.get_editor_property('guid') for d in wp.get_actor_descs()])
actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
counts={}
for a in actors:
    key=a.get_class().get_name(); counts[key]=counts.get(key,0)+1
assert counts.get('RackThermalActor')==16 and counts.get('SpineCabinetActor')==2, counts
assert unreal.EditorLoadingAndSavingUtils.save_map(world,'/Game/Maps/L_Test')
registry=unreal.AssetRegistryHelpers.get_asset_registry()
options=unreal.AssetRegistryDependencyOptions(True,True,True,True,True)
deps=[str(p) for p in registry.get_dependencies('/Game/Maps/L_Test',options)]
content=Path(unreal.Paths.project_content_dir())
# External actor packages do not necessarily expose a browsable asset to EditorAssetLibrary.
missing=[p for p in deps if p.startswith('/Game/') and not
         (content / (p[len('/Game/'):]+'.uasset')).is_file() and not
         (content / (p[len('/Game/'):]+'.umap')).is_file()]
report={'actors':counts,'dependencies':deps,'missing_game_dependencies':missing}
(Path(unreal.Paths.project_saved_dir())/'LTest_References.json').write_text(json.dumps(report,indent=2),encoding='utf8')
assert not missing, missing
unreal.log('LTEST_REFERENCE_PASS '+json.dumps(counts))
