# CRAH 외형 모델 — 2026-10-14

닫힌 인로우 캐비닛과 벽면 CRAH를 기존 CDU 제작 방식으로 만들었다.
STULZ 사진은 **외형 참고만, 제조사 사양 복제 아님**. 크기는 프로젝트의 물리 상자 기준이며
제조사 실측·CAD·성능 사양을 나타내지 않는다. 로고·상표 띠·사진 텍스처를 사용하지 않았다.

## 좌표와 모델

원점은 바닥 중심, +X 전면/토출, +Y 폭, +Z 위. Blender/GLB는 m, FBX 정점은 cm로 내보낸다.
손잡이·HMI·그릴까지 명목 상자 안에 들어간다. 7개 `MC_CRAH_*` 단색 PBR 재질과 UV0를 공유한다.

| 모델 | 명목 XYZ (m) | 측정 min → max (m) | 정점 / 삼각형 |
|---|---|---|---|
| InRow | 1.068 × 0.6 × 2.236 | (-0.534000039, -0.300000012, 0) → (0.534000039, 0.300000012, 2.236000061) | 5,540 / 10,060 |
| Perimeter | 0.9 × 2 × 2 | (-0.449999988, -1, 0) → (0.449999988, 1, 2) | 5,660 / 10,476 |

최대 명목 경계 오차는 각각 6.10352e-8 m / 1.19209e-8 m. 기하 경계 검사 한계는 1e-6 m,
삼각형 한계는 각 15,000이다. 물리 잔차 허용 기준과 무관한 아트 검증이다.
전체 값은 `verification.json`에 기록한다.

- 인로우: 전면 팬 3개와 격자, HMI·손잡이·잠금장치, 후면의 촘촘한 흡기 그릴. 측면 평판.
- 벽면: 상부 서비스 도어 3장·HMI, 하부 전면 팬 3개·보호 그릴, 윗면 전체 흡기 그릴.
  토출은 +X 하부이며 이중 바닥 토출이 아니다.
- `CRAH_Reference.blend`는 두 편집 컬렉션과 스튜디오를 포함한다. 기본 표시 컬렉션은 인로우이며,
  벽면을 편집하려면 해당 컬렉션의 뷰포트/렌더 숨김을 해제한다.
- `crah_*_preview.png`, `crah_inrow_rear.png`, `crah_perimeter_top.png`는 **Blender 렌더**다.

## 언리얼 연결

메시 두 개는 `/Game/Connections/SM_CRAH_InRow`, `/Game/Connections/SM_CRAH_Perimeter`,
재질은 `/Game/Connections/Materials/MC_CRAH_*`다. 기존 Connections AlwaysCook 설정을 쓴다.

`ACrahUnitActor.bUseReferenceModel` 기본값은 true. Kind별 메시를 `SizeM / 명목 크기`로 스케일한다.
옵션을 끄거나 에셋 로드에 실패하면 기존 큐브로 돌아간다. 기존 RefreshVisuals 경로를 사용하며
SupplyArrow와 NoCollision을 유지한다. `ADataCenterSimulationActor.bUseCrahReferenceModels`는
자동 배치 표시의 같은 옵션이다. 공통 해석은 `Visualization/CrahVisualModel.h`에 있다.
공유 `CrahMesh`는 계속 큐브여서 컨테인먼트 판에 새 장비 메시를 적용하지 않는다.

SizeM·Kind·VolumeFlowM3s 기본값, 고체/덕트 생성, 코일·팬 계산, 저장 v2, L_Test/외부 액터는 변경하지 않았다.
단, 소스 변경 범위 확인은 물리 결과의 비트 단위 동일성 실행 검증을 대신하지 않는다.

## 재생성 / 설치 / 검증

저장소 루트에서 실행하며 프로그램과 저장소 경로는 해당 PC에 맞춘다.
재생성은 이 폴더의 생성물을 덮어쓰므로 수동 편집본은 별도 이름으로 보존한다.

```powershell
& '<Blender>/blender.exe' -b --python Art/CRAH/build_crah.py
& '<UE5.8>/Engine/Build/BatchFiles/Build.bat' VRG_TermProjEditor Win64 Development '-Project=<repo>/VRG_TermProj.uproject' -WaitMutex
& '<UE5.8>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' -run=pythonscript '-script=<repo>/Tools/install_crah_model.py' '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
& '<UE5.8>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' -run=pythonscript '-script=<repo>/Tools/verify_crah_model.py' '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
& '<UE5.8>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' '-ExecCmds=Automation RunTests VRG.Interaction.Layout.Crah+VRG.Interaction.Layout.PerimeterCrahAddition+VRG.Coupling.CrahUnitCarriesAirLoad+VRG.Coupling.PlacedCrahActors;Quit' -unattended -nopause -nosplash -NullRHI -log
```

`verify_crah_model.py`는 에셋 검사 후 임시 액터 두 종류에서 옵션 on/off/on, 스케일·피벗,
입력 불변·토출 화살표·충돌 비활성을 검사하고 액터를 제거한다. 레벨을 저장하지 않는다.
에셋만 검사하려면 `-script="<repo>/Tools/verify_crah_model.py --assets-only"`를 쓴다.
에셋 부재 시 fallback, 자동 배치, 물리 결과 동일성은 별도의 실행 검증이 필요하다.

## 실제 수행 범위와 남은 검증

- Blender 5.2.2: 생성·FBX/GLB 내보내기·4방향 렌더 성공. 경계·삼각형·UV 검사 통과.
- 이 노트북에는 UE 5.7.3만 설치되어 있다. 원본 `.uproject`의 5.8 연결은 유지했다.
  `Saved/CRAH_UE57`에 에셋 전용 임시 프로젝트를 만들어 신규 메시/재질만 임포트했다.
  5.7 제작 에셋의 5.8 로드는 지원 방향이지만 원본 5.8 프로젝트를 5.7로 다운그레이드하지 않는다.
- UE 에셋 측정 결과는 `ue_assets_verification.json`에 기록한다. 이것은 에셋 전용 검사이며
  VRG 코드·액터·물리·L_Test를 실행한 결과가 아니다.
  최종 `-NullRHI` commandlet Exit0/PASS. 재질 각 7개, UV 각 1개, 단순 충돌 각 0개.
  임포트 후 삼각형은 10,036 / 10,452개(각 24개 퇴화 삼각형 제거), 렌더 정점은
  21,060 / 23,343개다. UV/법선 경계 분리 때문에 Blender 원본 정점 수와 다르다.
  Blender 경계 대비 최대 임포트 차는 5.24521e-6 / 8.82149e-6 cm(허용 0.001 cm).
  검증한 메시 2개·재질 7개를 원본 Content에 복사하고 SHA-256 동일성을 확인했다.
- 별도 UE 5.7 에디터에서 모델 2개를 배치한 임시 맵 생성 로그는 확인했다.
  요청한 Alt+Print Screen을 1회 실행했으나 클립보드 읽기가
  `Requested Clipboard operation did not succeed`로 실패했다. 요청에 따라 캡처를 반복하지 않았다.
  따라서 UE 화면의 시각 검증은 미완료이며 제공 PNG 4개는 Blender 렌더다.
- **미실행:** UE 5.8 Editor Development 빌드, 요청한 영향 Automation, 액터 옵션/fallback 실행,
  풍량·UA·팬·열수지·발산 비트 동일성, L_Test 배치 버튼 두 종류 및 자동 CRAH 8대 확인.
  UE 5.8 설치 PC에서 위 명령과 L_Test -game 확인을 마쳐야 한다. 전체 Automation은 실행하지 않았다.

내부 코일·배관·제어반, 배관 연결부, 동작 HMI, 회전 팬 애니메이션, LOD, 충돌 메시를 만들지 않았다.
팬은 외형 표시용 고정 메시다. 모델 세부 형상이 물리 고체·흡기/토출 면을 변경하지 않는다.

## 참고 출처

제품 페이지: [CyberRow](https://www.stulz.com/products/detail/cyberrow/),
[CyberAir 3PRO](https://www.stulz.com/products/detail/cyberair-3pro-dx/).
사용자 제공 이미지 URL 목록이며 사진은 저장소에 포함하지 않는다.

1. https://www.stulz.com/fileadmin/_processed_/a/5/csm_csm_Stagebild_2400x1120_CyberRow_e7acde15f3_0d3f0d1d78.jpg
2. https://www.stulz.com/fileadmin/_processed_/f/2/csm_Produktbilder_CyberRow_Frontal_Breiten_46e43bf3b6.png
3. https://www.stulz.com/fileadmin/_processed_/5/c/csm_2029_16930-bearb_852px_448b70f98e.jpg
4. https://www.stulz.com/fileadmin/_processed_/8/f/csm_Bild_CW-System_14d6b4fc47.png
5. https://www.stulz.com/fileadmin/_processed_/1/b/csm_CyberAir_3PRO_CW_800x800px_bd10cd0053.jpg
6. https://www.stulz.com/fileadmin/_processed_/c/4/csm_ASR_Front_1400x900px_e957b86831.jpg
7. https://www.stulz.com/fileadmin/_processed_/d/9/csm_EC_Ventilator_RadiCal2_1400x900px_17a55d1bbb.jpg

엔진 호환성: https://dev.epicgames.com/documentation/unreal-engine/versioning-of-assets-and-packages-in-unreal-engine
