# AGENTS.md

AI 코딩 에이전트(Codex, Claude Code 등)가 이 저장소에서 작업할 때의 지침이다.

## 먼저 읽을 것 (순서대로, 예외 없음)

1. **`CLAUDE.md`** — 팀이 합의한 규약(단위·이름·시계·스레드·SoA), 설계 결정 D1~D10,
   측정된 오차 기준표, 물리 참고값, 빌드·테스트 명령. **작업 시작 전에 전문을 읽는다.**
   이 파일은 요약이 아니라 포인터다. 규약을 여기에 복사하지 않는다 — 두 곳으로 갈라진다.
2. **`남은작업.md`** — 지금 기준의 진행 상황, 남은 단계(S5 → S6 → S7 → S2b), 미상환 부채.
   다음에 할 일은 이 문서에서 고른다.
3. `구현순서.md` — 2026-09-13 설계 문서. 체크박스는 갱신되지 않았다. 배경 참고용.

세 문서가 충돌하면 우선순위는 `CLAUDE.md` > `남은작업.md` > `구현순서.md`다.

## 절대 규칙

- **단계를 건너뛰지 않는다.** 완료 조건 = "해당 단계 물리 테스트가 UE Automation Test로
  전부 통과 + 측정된 오차 기준이 정규화 기준·단위와 함께 `CLAUDE.md`에 기록됨".
- **D1~D10을 되돌리지 않는다.** 되돌리려면 팀 합의가 필요하다. 요청이 D1~D10과
  충돌하면 구현하지 말고 먼저 지적한다.
- **완료를 "화면이 자연스럽다"로 판정하지 않는다.** 보존 검사로 판정한다.
  그리고 보존 검사는 스케일 오류를 못 잡으므로 타당성 안전망(ΔT 범위, 펌프 전력 비율 등)을
  따로 둔다. 실제로 두 번 잡혔다.
- **오차 기준을 미리 정하지 않는다.** 측정한 뒤 여유를 둬서 정하고, 정규화 기준과 단위를
  함께 `CLAUDE.md` 표에 적는다.
- **랙이 두 종류다** (15 kW 하이브리드 / GB200 NVL72). 파라미터를 섞지 않는다.
- 솔버 내부에 cm를 넣지 않는다. 온도는 K로 저장한다. `Q`라는 이름을 쓰지 않는다.
- 커밋에 `Binaries/`, `Intermediate/`, `DerivedDataCache/`, `Saved/`, `.idea/`를 넣지 않는다.

## 빌드 / 테스트

엔진 경로는 `CLAUDE.md` 기준 `D:\UE_5.8`이다. **다른 PC에서 작업하면 경로가 다르다** —
자기 설치 경로로 바꿔 쓰고, `CLAUDE.md`의 경로는 원 작성자 기준이므로 고치지 않는다.

빌드:
```
<UE>\Engine\Build\BatchFiles\Build.bat VRG_TermProjEditor Win64 Development -Project="<repo>\VRG_TermProj.uproject" -WaitMutex
```
에디터가 열려 있으면 Live Coding 때문에 막힌다. 에디터를 닫거나 Ctrl+Alt+F11.

테스트 (현재 55개 전부 통과):
```
<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\VRG_TermProj.uproject" -ExecCmds="Automation RunTests VRG;Quit" -unattended -nopause -nosplash -NullRHI -log
```
결과는 stdout이 아니라 `Saved/Logs/VRG_TermProj.log`에 남는다.
`Test Completed. Result={Success}`와 `BeginEvents`/`EndEvents` 사이의 측정값을 확인한다.

**빌드·테스트를 돌리지 않았으면 "통과했다"고 쓰지 않는다.** 환경 때문에 못 돌렸으면
그렇게 적는다.

## 코드 배치

`Source/VRG_TermProj` 안에서 책임별로 나눈다 (표는 `CLAUDE.md`의 "코드 구성" 참고).
물리 계산은 일반 C++ 자료형으로 구현하고, 액터·UI는 입력 전달과 결과 표현만 한다.
새 물리 기능은 `Source/VRG_TermProj/Tests`에 Automation Test를 함께 넣는다.
`Niagara`, `UMG`, `RenderCore`, `RHI`는 실제로 쓰는 단계에서 Build.cs에 추가한다.

## 작업 방식

- 구현 전에 가정을 말한다. 해석이 여러 개면 고르지 말고 제시한다.
- 요청받은 것만 고친다. 인접 코드를 "개선"하지 않는다. 기존 스타일에 맞춘다.
- 단계를 끝내면 `남은작업.md`의 체크박스와 `CLAUDE.md`의 오차 기준표·참고값을 갱신한다.
  이 갱신이 단계 완료의 일부다.
- PR/커밋에는 구현 내용, 실행 방법, 검증 결과, 알려진 한계를 적는다.
