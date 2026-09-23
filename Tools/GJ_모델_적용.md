# GJ 모델의 프로젝트 적용 (2026-09-23)

## 열어서 확인하기

`VRG_TermProjEditor`를 빌드한 뒤 `/Game/Maps/L_Test`를 연다. World Partition에서
랙이 있는 영역을 로드하면 기존 `RackThermalActor`가 새 모델을 생성한다.
16대의 랙 액터와 기존 CDU를 유지하고, 랙마다 프레임 1개·노드 18개·스위치 1개를 표시한다.
노드를 탈착하면 해당 노드가 전면으로 이동하고 연결 하네스는 숨겨진다.
데이터 포트와 스위치 포트는 후면에 있다 (아래 "포트 배치").

- 랙 Details → Visualization / GJ → **Use Gj Models**, **Refresh Visuals**.
- 방 Details → Visualization / GJ → **Use Gj Connections**, **Refresh Gj Connections**.
- 같은 줄의 랙이나 CDU를 이동하면 뷰포트 Tick에서 곡선 연결을 다시 만든다.
  기존의 줄 분류 규칙(수평 축 방향·행 위치)과 가장 가까운 CDU 선택을 사용한다.
- `SpawnRackGrid`로 새로 만든 GB200 랙도 방의 GJ 사용 설정을 따른다.
- GJ 사용을 끄면 기존 기본 도형 표현으로 돌아간다. 18슬롯이 아닌 랙은 기존 표현을 유지한다.

## 원본과 실행 에셋

| 경로 | 역할 |
|---|---|
| `Art/GJ_PIPE&CABLE/GB200_Connections.blend` | 편집 가능한 원본 |
| `Art/GJ_PIPE&CABLE/reports/connection_manifest.json` | 포트·좌표·연결 기준 |
| `Art/GJ_PIPE&CABLE/runtime_exports` | 노드별 고정 하네스 18개와 매니폴드 FBX |
| `Content/ExistingGB200` | 랙·노드·스위치 및 원래 재질 |
| `Content/Connections` | 곡선용 몸체·피팅·하네스 및 재질 |
| `Source/VRG_TermProj/Visualization/GjConnectionMesh.*` | 곡선 메시 생성 |

기존 uasset의 재질 참조를 보존하려고 샘플과 동일한 패키지 경로를 사용한다.
샘플 프로젝트의 레벨이나 플러그인은 복사하지 않는다. 문자열로 로드하는 두 콘텐츠
디렉터리는 `DefaultGame.ini`의 AlwaysCook 대상이다. 패키징 실행 검증은 별도다.

Blender 원본은 m, UE 에셋 좌표는 cm다. UE에서 +X 전면, +Z 위쪽,
랙 피벗은 바닥 중심이다. 랙 크기 XYZ는 1.068 × 0.600 × 2.236 m이다.
사진 우선으로 만든 Quantum InfiniBand 스위치이며 NVLink 스위치로 간주하지 않는다.
추정 외형·피팅 치수는 원본 문서의 한계를 그대로 가진다.

## 포트 배치: 데이터는 후면 (2026-09-23)

데이터 포트를 냉각수와 같은 **후면(로컬 −X)**으로 옮겼다. **랙은 돌리지 않았다.**
+X는 흡기면이고 노드 탈착 방향이며, 두 번째 줄은 180°로 마주보는 핫/콜드 아일이라
랙을 돌리면 열 계산이 바뀐다. 이번 변경은 외형뿐이다. 열모델·수력망·D1~D10은 그대로다.

| 항목 | 전 | 후 (Blender m, 랙 로컬, +X 전면) |
|---|---|---|
| 노드 IO 케이지 8개 | 전면 x≈+0.41, y −0.185~+0.179 | 후면 가로보, 노드 로컬 x −0.422~−0.404, y −0.1155~+0.1155 (0.033 간격) |
| 노드 Data01 끝점 | (+0.476, −0.185, z+0.023), 바깥 +X | (−0.365, −0.1155, z+0.023), 바깥 −X |
| 스위치 | 피벗 (+0.134, 0, 1.91), 회전 0 | 피벗 (−0.134, 0, 1.91), **요 180°** (역방향 장착) |
| 스위치 포트 | x +0.476, 칸 k의 y −0.197+(k−1)·0.022 | x −0.476, 칸 k의 y +0.197−(k−1)·0.022 (요 180°로 거울상) |
| 노드↔스위치 하네스 | 전면 x +0.59~+0.69, 측판 밖 y −0.275~ | 후면 수직 리본, x −0.545~−0.630, y −0.1155 |
| 랙 간 데이터선(UE) | 전면으로 20 cm 뺀 뒤 위로 | 후면으로 20 cm 뺀 뒤 위로 (핫 아일 쪽) |

- 매니폴드는 랙 로컬 **y = −0.245(공급), +0.245(회수)로 양쪽 옆에 있다**. 그래서 하네스는
  "반대쪽 옆"이 아니라 두 매니폴드 사이의 **후면 중앙 통로**로 올렸다 (요청 참고 이미지와 같은 구성).
  냉각 호스·매니폴드와의 표면 간격 최소 **70.8 mm** (중심선 거리 − 두 반지름, `check_gj_rear_ports.py`).
- 노드 뒤 가로보에는 IO 자리마다 24 × 13 mm 구멍을 뚫었다. 케이지 테두리가 뒤로 2 mm 나와
  노드 메시 경계 min X가 −42.0 → **−42.2 cm**로 바뀐다. 나머지 경계는 그대로다.
- 앞 패널(`PerforatedFrontIO`)은 원래 타공 베젤이라 포트를 빼도 빈 구멍이 생기지 않는다.
- 노드를 탈착하면 그 노드의 하네스(데이터 포함)는 숨겨지고, 전면에는 원래 케이블이 없다.
- 공유 좌표는 `Art/GJ_PIPE&CABLE/scripts/rear_data_layout.py` 한 곳에 있다. C++(`RackThermalActor`의
  스위치 배치, `DataCenterRoomActor`의 랙 간 포트)의 숫자는 이 파일과 같아야 한다.
- 원본 생성기 `build_connections.py`도 같은 배치로 고쳤지만, 참조하는 `GB200_Modular_Kit.blend`가
  저장소에 없어 이 PC에서는 실행하지 못했다. 기존 `.blend`는 `migrate_rear_data.py`로 제자리에서 고친다
  (다시 실행해도 결과가 같다).

검증 (2026-09-23, UE 5.8.2 `D:/UE_5.8`, Blender 5.2.2):
`validate_ports.py` 커넥터 꼬리 정합 226곳 최대 6.0e-8 m, `check_gj_rear_ports.py` PASS
(데이터 끝점 80개 전부 x < 0, 최대 x −0.365 m, 데이터 경로 최대 x −0.409 m, 스위치·노드 몸체 관통 0),
설치·재로드 PASS(랙 16, 노드 288, 곡선 구간 648), 전체 `VRG` Automation **71건 성공 / 실패 0**
(3건은 엔진 외부 HTTP 확인 타임아웃 경고 동반, 코드와 무관).
`VRG.Visualization.GJ.ExistingActorsAndHydraulics`에 스위치 역방향 장착, 랙 회전 불변,
랙 간 데이터선 끝점이 전부 랙 로컬 x < 0이고 지붕 위를 지나지 않음을 추가했다.
D3D12 에디터 스크린샷: `Art/GJ_PIPE&CABLE/previews/UE_GJ_Front.png`, `UE_GJ_Rear.png`.
이 검사는 겉보기 간격 검사이며 케이블 장력·굽힘 반경·충돌 해석이 아니다.

## 빈 U 블랭크와 PatchManager (2026-09-23)

**랙이 비어 보였던 이유.** 48U 프레임(설치 높이 0.081~2.186 m)에 컴퓨트 트레이 18U와 스위치 1U만 있었다.
실제 NVL72에 있는 NVLink 스위치 트레이 9장과 전원 셸프는 열모델(105.3 kW)에도 없어서 넣지 않았다.
이번에는 팀 결정대로 **빈 U 28칸을 1U 블랭크 판으로만** 막았다 (아래 7 + 중간 16 + 위 5).
블랭크는 표시용이며 열·공기·수력에 영향이 없다. NVLink 트레이·전원 셸프를 넣으려면 발열을 열모델에
넣을지부터 팀이 정해야 한다 (105.3 kW 기준값과 테스트 기준이 바뀐다).

- `SM_GJ_Blank1U`: 19인치 1.5 mm 판 + 나사 2개. `bake_gj_rack_harness.py`가 만들고 설치 스크립트가 가져온다.
- 캐비닛당 `InstancedStaticMeshComponent` 1개(드로 호출 1개). 1U의 정수배가 아닌 틈(스위치가 U 격자에서
  6.7 mm 어긋나 있다)은 판을 고르게 늘려 채운다. 좌표는 `Visualization/GjRackLayout.h`.

**PatchManager.** `Visualization/PatchManager.*` (월드 없는 순수 C++), 룸 액터가 결과를 그린다.

| 구성 | 내용 |
|---|---|
| 포트 장부 | (장비, 포트)를 한 번만 점유한다. 두 번째 점유는 거부되고 개수를 센다 |
| 업링크 | 랙 스위치 후면 윗줄 가운데 `CablesPerRack`개(기본 4, 8~11번 칸) -> 같은 줄 스파인 캐비닛의 빈 포트를 순서대로 |
| 스파인 캐비닛 | `ASpineCabinetActor`. 같은 GJ 프레임 + 역방향 Quantum 스위치 `NumSwitches`대(기본 4, 144포트) + 블랭크 |
| 스파인 간 링크 | 평행한 이웃 줄의 스파인끼리 `SpineLinksPerRowPair`개(기본 4), 루프의 가까운 끝을 지나 건넌다 |
| 트레이 | 줄마다 핫 아일 위(뒷면에서 `TrayStandoffM` 0.27 m, 높이 `CableTrayHeightM` 2.85 m) 사다리 트레이. 평행한 줄은 끝을 맞춰 한 루프로 닫는다 |
| 경로 | 포트 -> 뒤로 빠져 자기 레인까지 -> 트레이로 수직 상승 -> 트레이를 따라 -> 상대 포트 위에서 하강. 케이블마다 트레이 안 레인(가로 10줄, 층 1.2 cm)을 따로 가진다 |
| 미연결 | 스파인 없는 줄, 스파인 포트 부족, 평행하지 않은 줄의 스파인 링크는 **조용히 버리지 않고** 사유와 함께 화면·요약에 적는다 |

- L_Test: 스파인 2대(각 줄 북쪽 끝 y=5.28 m), 케이블 68개(업링크 64 + 스파인 간 4), 중복 점유 거부 0, 미연결 0.
- 룸 액터 Details -> `Spawn Spine Cabinets`: 스파인이 없는 줄 끝에 하나씩 놓는다(이미 있으면 건너뜀).
  설치 스크립트가 이것을 부르고 `IsPatchComplete()`를 확인한다. 줄 간격이 불규칙하면(L_Test B줄) 가장 좁은 랙 간격을 쓴다.
- 기존 "인접 랙끼리 4가닥" 링크는 PatchManager 업링크로 대체했다.
- **한계.** 표시용 배선 계획이다. 대역폭·지연·케이블 길이 제한·장력·굽힘 반경·케이블끼리 간섭은 계산하지 않는다.
  스파인 캐비닛은 발열원이 아니고 공기 격자에서 고체도 아니다 (실제 스파인 스위치는 대당 1~2 kW).
  2D 평면도 UI는 이번 범위가 아니다.

검증: `VRG.Visualization.Patch.LedgerAndRouting` 신규 — 이중 점유 거부, 두 줄 x 3랙 전부 연결·포트당 케이블
끝 하나·경로가 캐비닛 평면 투영을 관통하거나 지붕 위를 지나지 않음(2 cm 간격 표본, 포트 꼬리 구간 제외),
용량 초과(40 > 36포트)와 스파인 없는 줄의 미연결 보고. `ExistingActorsAndHydraulics`에 스파인 연결·블랭크 28개 추가.
전체 `VRG` Automation **72건 성공 / 실패 0** (2건 엔진 HTTP 타임아웃 경고). 설치·재로드 PASS (곡선 구간 908).
스크린샷: `Art/GJ_PIPE&CABLE/previews/UE_GJ_Front.png`(블랭크), `UE_GJ_Rear.png`, `UE_GJ_Patch.png`(루프 조감).

**프레임 측정** (L_Test, `-game` 1280x720, 6초, 같은 명령, 중앙값 ms — 측정값이며 보장값이 아니다, D10):

| | Frame | Game | Draw | GPU |
|---|---|---|---|---|
| 패치·블랭크·스파인 끔 | 132.04 | 132.01 | 68.07 | 20.40 |
| 켬 | 133.31 | 133.28 | 70.65 | 21.30 |

이번 추가분은 Draw 약 +2.6 ms다(같은 바이너리 반복 편차 약 1 ms).
**정정 (같은 날 2-1 작업에서 확인):** Draw 68 ms의 원인은 GJ 메시가 아니라 **GPU 압력 Jacobi 경로**였다.
그 경로는 덩어리마다 `BlockUntilGPUIdle`로 렌더 스레드를 붙잡고, 프레임당 1600회를 돌았다. 같은 장면에서
GPU Jacobi Draw 67.62 ms / CPU Jacobi 16.23 ms / 멀티그리드 3.81 ms로 확인했다 (CLAUDE.md "2-1" 절).

## 재생성

에디터를 닫고 프로젝트 루트에서 PowerShell로 실행한다. 경로는 설치 위치에 맞춘다.
설치 스크립트는 L_Test의 기존 18슬롯 랙들을 수정하고 저장하므로 의도적으로 재적용할 때만 실행한다.
Blender 원본은 5.2로 저장돼 있어 **Blender 4.x 이하에서는 열리지 않는다** ("not a blend file").
`bake_gj_rack_harness.py`는 하네스 18개·매니폴드와 함께 노드 메시(`runtime_exports/ExistingGB200`)도
내보내고, 설치 스크립트가 그것을 `/Game/ExistingGB200`에 원래 재질로 다시 가져온다.

```powershell
# 원본 .blend를 고칠 때만: 데이터 측 후면 배치 적용 + manifest·미리보기 갱신
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' -b --python 'Art/GJ_PIPE&CABLE/scripts/migrate_rear_data.py'
python 'Art/GJ_PIPE&CABLE/scripts/validate_ports.py'; python Tools/check_gj_rear_ports.py
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' -b --python Tools/bake_gj_rack_harness.py
& 'E:/UE_5.8/Engine/Build/BatchFiles/Build.bat' VRG_TermProjEditor Win64 Development "-Project=$PWD/VRG_TermProj.uproject" -WaitMutex
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' "$PWD/VRG_TermProj.uproject" -run=pythonscript "-script=$PWD/Tools/install_gj_models.py" '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' "$PWD/VRG_TermProj.uproject" -run=pythonscript "-script=$PWD/Tools/verify_gj_models.py" '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
# 전면·후면 스크린샷 (실제 RHI, 저장하지 않음). -ExecutePythonScript는 스크립트 직후 에디터를 닫으므로 py 명령으로 넘긴다.
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe' "$PWD/VRG_TermProj.uproject" "-ExecCmds=py $PWD/Tools/gj_port_screenshots.py" -nosplash
```

## 구현 범위와 한계

외부 냉각 배관과 랙 간 데이터선은 `USplineMeshComponent`로 길이와 굽힘이 변한다.
고정 노드 하네스는 반복되는 형상을 합친 메시이며, 독립적인 편집 핸들을 노출하지 않는다.
현재 자동 경로에는 장애물 회피·중력 처짐·케이블 장력 계산이 없다.
랙 간 데이터선은 PatchManager가 줄 끝 스파인 캐비닛으로 배선한다 (아래 절).
CDU에는 냉각수 공급·회수관을 연결한다. 데이터선을 CDU 냉각 포트에 연결하지 않는다.

외부 배관 색은 공급/회수 역할, 밝기는 기존 수력망에서 계산된 유량이다.
노드 하네스는 역할별 고정 재질이며 개별 호스 유량을 새로 푼 것이 아니다.
외형 곡선 길이·엘보에 따른 저항 재산정도 이번 범위에 포함하지 않는다.
열모델·D1~D10·발열 입력은 유지되며, 랙 AABB는 새 외형 치수에 맞춰 바뀐다.
온도별 부품 색 덮어쓰기는 GJ 모델에서 생략하고 기존 수치 표시를 사용한다.
실제 HMD 프레임률과 패키징 결과는 별도 검증이 필요하다.

## 해결: L_Test 공기장 압력 수렴 실패 (2026-09-23)

원인은 모델 교체가 아니라 압력 해법이었다. 랙을 고체로 막은 불규칙한 영역에서 완화 Jacobi는 400회 상한 안에
1e-5 근처도 못 간다(같은 문제의 테스트에서 4000회에 1.9e-6). 멀티그리드 전처리 CG로 바꾼 뒤
목표 1e-6에서 1500스텝 예열 + 8초 측정 동안 **수렴 실패 0회**다. 자세한 수치는 CLAUDE.md "2-1" 절.
