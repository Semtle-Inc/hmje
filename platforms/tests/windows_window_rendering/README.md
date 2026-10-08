# Windows 그라데이션과 창 시작 회귀 검사

네이티브 검사 프로그램은 실제 Win32 호스트와 D3D 렌더러를 포함합니다. 하드웨어 D3D11의 방사형·타원형 픽셀, 1픽셀 그라데이션, 선형 그라데이션, 제목·스타일 반복 설정, 유연한 초기 창 크기, 화면 밖이나 과도하게 큰 창의 복원을 검사합니다. 화면 캡처로 깜박임을 추정하는 대신 `WM_SETTEXT`·`WM_STYLECHANGED` 횟수를 측정합니다.

LLVM과 Windows SDK가 설치된 Windows 대화형 데스크톱에서 실행합니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File platforms/tests/windows_window_rendering/run.ps1 -OutputDirectory C:/hmje-window-probe -Clang 'C:/Program Files/LLVM/bin/clang.exe'
```

스크립트는 ARM64·x64 검사 프로그램을 빌드하고 각각 100%·200% 배율에서 실행합니다. 각 실행은 별도의 로컬 상태 디렉터리를 사용합니다. 프로세스 환경 설정은 복원하며 시스템 DPI 설정은 변경하지 않습니다. 각 검사 프로그램은 자신이 만든 창만 닫습니다.

검사 범위는 렌더러와 창 동작이며 IDE의 UI 전체를 검사하지 않습니다. HLSL 소스와 이를 포함한 C 문자열은 같은 내용을 유지해야 합니다.
