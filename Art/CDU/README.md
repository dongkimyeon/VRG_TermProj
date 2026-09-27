# 참고 이미지 기반 CDU (2026-09-27)

사용자가 첨부한 GB200 랙 이미지의 왼쪽 독립 CDU를 참고했다. `reference.png`는 제공된 확대 이미지다.
검정 캐비닛, 양문 패널과 중앙 이음매, 분할 상판, 작은 흰 HMI, 가로 손잡이,
원형 통풍구 3개(실제 구멍·그릴), 잠금장치·나사·하부 서비스 패널을 모델링했다.
이미지로 확인할 수 없는 내부 펌프/열교환기는 만들지 않았다. 제조사 CAD나 제품 사양을 복제한 것은 아니다.

## 파일과 좌표

- `CDU_Reference.blend`: 부품별 편집 원본. `CDU_Editable_Parts`, `Preview_ports`, `Studio` 컬렉션.
- `SM_CDU_Reference.fbx`: 언리얼용 단일 메시, 11,432 삼각형, 6개 재질. UV0 포함.
- `CDU_Reference.glb`: 미터 단위 본체 교환 파일.
- `cdu_preview.png`, `cdu_front.png`: Blender 렌더. 화면 표시는 고정 그림이며 계측값이 아니다.
- `materials.json`, `verification.json`: 단색 PBR 재질 값과 메시 크기·정점 수.
- `ue_verification.json`: 저장된 L_Test에서 읽어 검사한 CDU 교체·입력 불변 결과.
- `build_cdu.py`: 원본/FBX/GLB/렌더 재생성.
- `replace_infrastructure_cdu.py`: 이전 배관 예제의 CDU 두 대 교체.

원점은 바닥 중심, +X 전면, +Y 폭, +Z 높이. nominal XYZ = **0.9 × 0.8 × 1.9 m**로
기존 `CoolingDistributionUnitActor.SizeM`에 맞췄다. 돌출 나사·표시창 포함 실제 메시 경계는
0.9025 × 0.8040 × 1.9000 m. FBX는 기존 GJ 파이프라인과 같은 cm 좌표·단위로 내보낸다.
제조사 실측 치수가 아니라 기존 프로젝트의 외형 기준이다.

## 언리얼 교체

`/Game/Connections/SM_CDU_Reference`와 `Materials/MC_CDU_*` 6개를 가져왔다.
`CoolingDistributionUnitActor`는 기본으로 새 본체를 사용한다. `SizeM` 변경에 따라 본체를 스케일한다.
Details → Visualization → **Use Reference Model**, **Refresh Visuals**로 확인할 수 있다.
옵션을 끄거나 에셋이 없으면 이전 기본 도형 외형을 쓴다.

기존 공급/회수 포트는 별도 컴포넌트로 유지해 `ConnectionHeightM`, `ConnectionSpacingM`을 따른다.
기본 포트는 로컬 `(0.45, -0.15, 0.25)`, `(0.45, +0.15, 0.25)` m.
Blender `Preview_ports`는 렌더용이며 본체 FBX에는 포함하지 않는다.
기존 L_Test 액터를 삭제하거나 레벨을 재저장하지 않았다. 펌프·열교환기·수력 입력도 바꾸지 않았다.
`Connections`는 기존 AlwaysCook 경로여서 별도 패키징 설정 변경이 필요하지 않다.

`Art/Infrastructure/VRG_Connections.blend`의 CDU 두 대도 새 본체로 교체했다.
이 예전 예제에서는 기존 .7 × 1.0 × 1.6 m 크기에 맞춰 스케일하고 배관 커브 714개를 보존한다.
재생성기·FBX·GLB·미리보기·manifest도 함께 갱신했다.
현재 UE 랙의 원본인 `Art/GJ_PIPE&CABLE/GB200_Connections.blend`는 랙/배선 원본이며 이 변경에 포함하지 않는다.

## 재생성/설치

프로젝트 루트에서 PowerShell로 실행한다. 프로그램 경로는 각 PC 설치 경로로 바꾼다.
생성 명령은 해당 아트 출력물을 덮어쓰므로 수동 편집본은 별도 이름으로 저장한다.

```powershell
& '<Blender>/blender.exe' -b --python Art/CDU/build_cdu.py
& '<Blender>/blender.exe' -b Art/Infrastructure/VRG_Connections.blend --python Art/CDU/replace_infrastructure_cdu.py
& '<Blender>/blender.exe' -b Art/Infrastructure/VRG_Connections.blend --python Art/Infrastructure/export_and_preview.py
& '<UE>/Engine/Build/BatchFiles/Build.bat' VRG_TermProjEditor Win64 Development '-Project=<repo>/VRG_TermProj.uproject' -WaitMutex
& '<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' -run=pythonscript '-script=<repo>/Tools/install_cdu_model.py' '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
& '<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' -run=pythonscript '-script=<repo>/Tools/verify_cdu_model.py' '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
```

## 검증

Blender 5.2.2 / UE 5.8.2, 현재 PC의 `C:/VRG_TermProj`에서 실행했다.

- Editor Win64 Development 빌드 성공.
- UE 가져오기: 경계 90.25 × 80.4 × 190 cm, 바닥 피벗·재질 슬롯 6개 확인.
- L_Test CDU 1대: 새 본체와 포트 2개, 기존 외형 전환, 위치/회전/크기와 입력 유지,
  기본 및 변경된 포트 높이/간격에서 공급 포트 좌표 검사 통과.
- `VRG.Visualization`: 5개 성공 / 실패 0 / 경고 0 (`Saved/Automation/CDU`).
- 배관 예제: 커브 714개 제어점 불변, FBX 내보내기/재가져오기 757개 일치,
  전체 경계 최대 차이 2.3842e-7 m (`Art/Infrastructure/verification.json`).

LOD, 충돌 메시, 내부 장비, HMI 동작은 구현하지 않았다. 본체는 기존 CDU처럼 충돌을 끄고
배치 선택은 기존 액터 로직을 따른다. 전체 Automation·재패키징·성능 기준선은 이번 변경에서 재측정하지 않았다.
