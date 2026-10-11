# 훈민정음 IDE 포함 자산 고지

훈민정음 IDE 패키지에는 IDE 화면을 그리는 데 필요한 자산이 함께 들어 있습니다.
아래 "IDE 전용 자산"은 [LICENSE](LICENSE) 제2조(적용 범위)에 따라 이 고지가
LICENSE보다 우선합니다.

## IDE 전용 자산

패키지 안 `HunminIDE.assets/` 폴더의 다음 파일입니다(macOS는 앱 묶음 안의 같은 자산).

- 아이콘 글꼴과 이름 자료: `fonts/EpicSemtle-Icons.ttf`, `fonts/SemtleUI-Icons.ttf`,
  `fonts/epicsemtle-icon-names-ko.json`
- 디자인 자료: `figma/` 폴더의 모든 파일
- 앱 아이콘·로고와 IDE 화면 이미지: `AppIcon.*`, `logo.png`, `cloud.png`, `dots*.png`,
  `file*.png`, `folder.png`

## 사용 조건

- 훈민정음 IDE를 설치하고 실행하는 데 필요한 범위에서만 사용할 수 있습니다.
- IDE에서 분리하여 복제, 수정, 배포, 판매하거나, 앱·웹사이트·문서·디자인 등 다른
  제작물에 사용할 수 없습니다.
- 글꼴 파일을 운영체제나 다른 프로그램에 설치하여 사용할 수 없습니다.
- 이 자산의 권리는 권리자에게 있으며, 이 고지는 어떠한 권리도 양도하거나 추가로
  허락하지 않습니다.

## 이 고지가 적용되지 않는 것

- 제삼자 글꼴과 자료(Pretendard, SUITE, D2Coding, Montserrat, Noto Color Emoji,
  Pretendard 비트맵 파생 글꼴, Unicode 데이터): 각 원래 라이선스와
  [제삼자 고지](THIRD_PARTY_NOTICES.md)를 따릅니다.
- 새 프로젝트 템플릿(`tpl-*.hmje`): 이 코드로 시작해 만든 앱은 다른 코드와 같이
  자유롭게 수정·배포할 수 있습니다.
- 훈민정음으로 만든 앱에 들어가는 런타임·라이브러리: [LICENSE-RUNTIME](LICENSE-RUNTIME)을 따릅니다.
  IDE 전용 자산은 여기에 포함되지 않습니다.

---

# Hunminjeongeum IDE Bundled Assets Notice

The Hunminjeongeum IDE packages include assets used to render the IDE itself. Under
Section 2 (Scope) of the [LICENSE](LICENSE), this notice governs the "IDE-only assets"
below and prevails over the LICENSE for them.

## IDE-only assets

The following files in the package's `HunminIDE.assets/` folder (on macOS, the same assets
inside the application bundle):

- Icon fonts and name data: `fonts/EpicSemtle-Icons.ttf`, `fonts/SemtleUI-Icons.ttf`,
  `fonts/epicsemtle-icon-names-ko.json`
- Design data: all files under `figma/`
- Application icon, logo, and IDE interface images: `AppIcon.*`, `logo.png`, `cloud.png`,
  `dots*.png`, `file*.png`, `folder.png`

## Terms

- They may be used only as needed to install and run the Hunminjeongeum IDE.
- They may not be extracted from the IDE and copied, modified, distributed, or sold, nor used
  in applications, websites, documents, designs, or any other work.
- The font files may not be installed in an operating system or used in other programs.
- All rights in these assets remain with their rights holders. This notice transfers no rights
  and grants no additional permission.

## Not covered by this notice

- Third-party fonts and data (Pretendard, SUITE, D2Coding, Montserrat, Noto Color Emoji, the
  Pretendard bitmap derivative, Unicode data) follow their original licenses and the
  [third-party notices](THIRD_PARTY_NOTICES.md).
- New project templates (`tpl-*.hmje`): applications you build starting from this code may be
  modified and distributed freely like any other code.
- Runtime and libraries embedded in applications built with HMJE follow
  [LICENSE-RUNTIME](LICENSE-RUNTIME). IDE-only assets are not part of them.
