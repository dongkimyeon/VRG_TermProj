# GJ 모델의 프로젝트 적용 (2026-09-23)

## 열어서 확인하기

`VRG_TermProjEditor`를 빌드한 뒤 `/Game/Maps/L_Test`를 연다. World Partition에서
랙이 있는 영역을 로드하면 기존 `RackThermalActor`가 새 모델을 생성한다.
16대의 랙 액터와 기존 CDU를 유지하고, 랙마다 프레임 1개·노드 18개·스위치 1개를 표시한다.
노드를 탈착하면 해당 노드가 전면으로 이동하고 연결 하네스는 숨겨진다.

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

## 재생성

에디터를 닫고 프로젝트 루트에서 PowerShell로 실행한다. 경로는 설치 위치에 맞춘다.
설치 스크립트는 L_Test의 기존 18슬롯 랙들을 수정하고 저장하므로 의도적으로 재적용할 때만 실행한다.

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' -b --python Tools/bake_gj_rack_harness.py
& 'E:/UE_5.8/Engine/Build/BatchFiles/Build.bat' VRG_TermProjEditor Win64 Development "-Project=$PWD/VRG_TermProj.uproject" -WaitMutex
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' "$PWD/VRG_TermProj.uproject" -run=pythonscript "-script=$PWD/Tools/install_gj_models.py" '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' "$PWD/VRG_TermProj.uproject" -run=pythonscript "-script=$PWD/Tools/verify_gj_models.py" '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
```

## 구현 범위와 한계

외부 냉각 배관과 랙 간 데이터선은 `USplineMeshComponent`로 길이와 굽힘이 변한다.
고정 노드 하네스는 반복되는 형상을 합친 메시이며, 독립적인 편집 핸들을 노출하지 않는다.
현재 자동 경로에는 장애물 회피·중력 처짐·케이블 장력 계산이 없다.
외부 데이터선은 인접 랙 스위치의 비어 있는 상단 포트에 최대 4가닥씩 연결한다.
CDU에는 냉각수 공급·회수관을 연결한다. 데이터선을 CDU 냉각 포트에 연결하지 않는다.

외부 배관 색은 공급/회수 역할, 밝기는 기존 수력망에서 계산된 유량이다.
노드 하네스는 역할별 고정 재질이며 개별 호스 유량을 새로 푼 것이 아니다.
외형 곡선 길이·엘보에 따른 저항 재산정도 이번 범위에 포함하지 않는다.
열모델·D1~D10·발열 입력은 유지되며, 랙 AABB는 새 외형 치수에 맞춰 바뀐다.
온도별 부품 색 덮어쓰기는 GJ 모델에서 생략하고 기존 수치 표시를 사용한다.
실제 HMD 프레임률과 패키징 결과는 별도 검증이 필요하다.

## 미해결: L_Test 공기장 압력 수렴 실패

2026-09-23 UE 5.8.2 / D3D12 실제 실행의 HUD에서 t=1.1 s에
압력 반복 상한 400회, 정규화 잔차 약 1.01e-4와
"압력 수렴 실패 - 속도장 믿지 말 것" 표시를 확인했다. **해결되지 않았다.**
71개 Automation Test 성공은 이 레벨의 장시간 압력 수렴을 보장하지 않는다.
모델 교체 전후 동일 조건 비교를 하지 않아 이번 교체가 원인인지는 확인되지 않았다.

관측 실행 명령(프로젝트 루트 PowerShell, 엔진 경로는 설치 위치에 맞춤):

```powershell
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' "$PWD/VRG_TermProj.uproject" /Game/Maps/L_Test -game -RenderOffscreen -windowed -ResX=1280 -ResY=720 -unattended -nosplash '-ExecCmds=vrg.Air.DrawSlice 0,vrg.Air.DrawVolume 0,vrg.Octree.DrawWireframe 0,vrg.Air.FrameDomain 0.7,vrg.Perf.Capture 6 GJModels quit' "-abslog=$PWD/Saved/GJ_Visual.log"
```

로컬 관측 자료는 `Saved/GJ_Visual.log`, `Saved/Profiling/VrgPerf-GJModels.*`,
`Saved/Screenshots/WindowsEditor/VrgShot-GJModels.png`에 있다.
`Saved/`는 팀 규약에 따라 커밋하지 않으므로 다른 PC에서는 위 명령으로 다시 수집한다.
화면 메시지를 숨긴 `GJModelsClean` 이미지는 수렴 진단 자료로 사용하지 않는다.

다음 담당자는 같은 초기조건·격자·압력 경로(CPU/GPU)·시간 간격에서 잔차 이력을 수집하고,
새 랙 AABB에 따른 고체 셀/흡배기 연결 변화와 초기 과도 상태를 분리해 비교해야 한다.
수렴 목표를 느슨하게 하거나 경고를 숨겨 완료 처리하지 않는다.
