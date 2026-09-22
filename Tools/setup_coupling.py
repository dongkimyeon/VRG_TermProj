# S4 통합 액터를 레벨에 배치하고 공기 도메인을 랙에 맞춘다.
# 통합 액터는 레벨에서 방/공기/랙을 자동으로 찾으므로 별도 연결 설정이 필요 없다.
#
# 월드 파티션 레벨이라 커맨드렛 모드에서는 외부 액터가 자동으로 안 올라온다.
# 액터 서술자를 전부 훑어 명시적으로 로드해야 한다.
import unreal

LEVEL = "/Game/Maps/L_Test"


def log(m):
    unreal.log("[VRG-SETUP] " + str(m))


world = unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
if world is None:
    unreal.log_error("[VRG-SETUP] 레벨을 열지 못했습니다: " + LEVEL)
    raise SystemExit(1)

wp = unreal.WorldPartitionBlueprintLibrary
descs = wp.get_actor_descs()
log("외부 액터 서술자 %d개" % len(descs))
guids = [d.get_editor_property("guid") for d in descs]
wp.load_actors(guids)
wp.pin_actors(guids)

actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = actor_sub.get_all_level_actors()
log("로드 후 액터 %d개" % len(actors))


def find(class_name):
    return [a for a in actors if a.get_class().get_name() == class_name]


racks = find("RackThermalActor")
air_list = find("AirFieldDebugActor")
room_list = find("DataCenterRoomActor")
sim_list = find("DataCenterSimulationActor")
log("랙 %d / 공기 %d / 방 %d / 통합 %d"
    % (len(racks), len(air_list), len(room_list), len(sim_list)))

if not racks or not air_list:
    unreal.log_error("[VRG-SETUP] 랙 또는 공기 액터가 없습니다. 중단합니다.")
    raise SystemExit(1)

# 1) 공기 도메인을 랙 전체에 맞춘다. 격자 밖으로 나간 열은 버려져 장부가 어긋난다.
air = air_list[0]
before_loc = air.get_actor_location()
air.fit_to_racks()
params = air.get_editor_property("grid_params")
nx = params.get_editor_property("num_cells_x")
ny = params.get_editor_property("num_cells_y")
nz = params.get_editor_property("num_cells_z")
h = params.get_editor_property("cell_size_m")
log("공기 격자 %dx%dx%d = %d셀, 셀 폭 %.3f m" % (nx, ny, nz, nx * ny * nz, h))
log("공기 액터 위치 %s -> %s" % (before_loc, air.get_actor_location()))

# 2) 통합 액터를 배치한다. 이미 있으면 그대로 둔다.
if sim_list:
    sim = sim_list[0]
    log("통합 액터가 이미 있습니다: " + sim.get_actor_label())
else:
    sim = actor_sub.spawn_actor_from_class(
        unreal.DataCenterSimulationActor, unreal.Vector(0.0, 0.0, 0.0))
    sim.set_actor_label("DataCenterSimulation")
    log("통합 액터 배치: " + sim.get_actor_label())

sim.rebuild_coupling()

# 3) 저장. 월드 파티션이라 외부 액터 파일도 같이 저장된다.
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log("저장 완료")
