# hmje-llm — GGUF 모델과 로컬 텍스트 추론

GGUF 파일·토크나이저·모델 바인딩과 Gemma 4 E2B-IT QAT Q4_0 텍스트 추론을 제공합니다. 수치 연산은 HmjeSem을 사용합니다.

## 사용

HMJE 1.0.0-beta.1 툴체인의 SDK에 포함됩니다. 소비 프로젝트의 모듈에 사용할 라이브러리를 의존성으로 선언하고, HMJE 소스에서 모듈을 가져옵니다. 내보내는 모듈은 HmjeLLM입니다.

```hmjep
모듈 App의
    원본 경로는 "Sources/App"이고,
    의존 라이브러리는 HmjeLLM이다.
```

```hmje
HmjeLLM을 가져온다.
```

프로젝트·산출물의 전체 구성은 [프로젝트 명세](../../docs/project.md), 이 트리에서 SDK를 빌드·설치하는 절차는 [소스 빌드](../../docs/building.md)를 참고하십시오.

## 기능

| 구분 | 제공 내용 |
| --- | --- |
| GGUF | 파일 소유 버퍼, 메타데이터와 텐서 읽기 |
| 토크나이저 | 텍스트 인코드·디코드와 대화 프롬프트 |
| 추론 | 실행기·K/V 캐시·그리디 생성 |
| 샘플링 | temperature·top-k·top-p와 토큰 스트리밍 |

## 지원 범위와 사용 조건

GGUF 읽기 기능과 개별 모델 실행 지원은 구분됩니다. Beta 1 텍스트 실행기는 위 Gemma 모델에 맞춰져 있으며, 임의의 GGUF 모델 실행이나 학습 API를 제공하지 않습니다. 모델 파일은 별도로 준비하고 해당 모델의 사용 조건을 확인합니다. Metal 사용 범위는 HmjeSem과 호스트 환경에 따릅니다.

## 문서와 소스

- [모듈·플랫폼별 빌드 구성](Project.hmjep)
- [구현 소스](Sources/)
- [추론 생명주기](docs/inference.md)
- [샘플링과 스트리밍](docs/sampler.md)
- [텐서 사용](docs/tensor_api.md)
- [캐시 버퍼](docs/circular_buffer.md)

## 버전과 라이선스

Beta 1의 API는 안정화 중입니다. 고정된 원본 커밋은 [소스 출처](../../SOURCES.md), 공통 변경 사항은 [릴리스 노트](../../RELEASE_NOTES-1.0.0-beta.1.md)를 확인하십시오. 앱에 포함하는 배포에는 [런타임·라이브러리 라이선스](../../LICENSE-RUNTIME)가 적용됩니다.
