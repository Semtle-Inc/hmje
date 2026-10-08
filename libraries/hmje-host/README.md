# hmje-host — 파일과 프로세스 호스트 API

HmjeFiles와 HmjeProcess 모듈로 파일·디렉터리·경로와 외부 프로세스를 제어합니다. 데스크톱 명령줄 도구와 편집기 등 호스트 프로그램에서 사용합니다.

## 사용

HMJE 1.0.0-beta.1 툴체인의 SDK에 포함됩니다. 소비 프로젝트의 모듈에 사용할 라이브러리를 의존성으로 선언하고, HMJE 소스에서 모듈을 가져옵니다. 내보내는 모듈은 HmjeFiles와 HmjeProcess입니다.

```hmjep
모듈 App의
    원본 경로는 "Sources/App"이고,
    의존 라이브러리는 HmjeFiles이다.
```

```hmje
HmjeFiles를 가져온다.
```

프로세스 기능을 사용할 때에는 `HmjeProcess` 의존성을 추가하고 `HmjeProcess를 가져온다.`로 가져옵니다.

프로젝트·산출물의 전체 구성은 [프로젝트 명세](../../docs/project.md), 이 트리에서 SDK를 빌드·설치하는 절차는 [소스 빌드](../../docs/building.md)를 참고하십시오.

## 기능

| 구분 | 제공 내용 |
| --- | --- |
| 파일 | 파일글읽기·파일글쓰기·파일새로쓰기와 존재 확인 |
| 디렉터리·경로 | 항목 열거·정렬·경로 결합·파일명 조회 |
| 프로세스 | 인자 전달·출력 수집·비동기 실행과 취소 |

## 지원 범위와 사용 조건

경로 처리는 POSIX·Windows용 모듈을 선택합니다. 파일 접근 권한과 실행 파일 검색은 운영체제 환경에 따릅니다. 프로세스에 전달할 인자를 개별 값으로 구성하고, 종료 상태·출력·취소 결과를 확인합니다. 모바일 샌드박스에서 데스크톱과 동일한 경로·프로세스 권한을 가정하지 않습니다.

## 문서와 소스

- [모듈·플랫폼별 빌드 구성](Project.hmjep)
- [구현 소스](Sources/)
- [파일 API](Sources/HmjeFiles/files.hmje)
- [프로세스 API](Sources/HmjeProcess/process.hmje)
- [호스트 스모크 검사](Tests/HostSmoke/main.hmje)

## 버전과 라이선스

Beta 1의 API는 안정화 중입니다. 고정된 원본 커밋은 [소스 출처](../../SOURCES.md), 공통 변경 사항은 [릴리스 노트](../../RELEASE_NOTES-1.0.0-beta.1.md)를 확인하십시오. 앱에 포함하는 배포에는 [런타임·라이브러리 라이선스](../../LICENSE-RUNTIME)가 적용됩니다.
