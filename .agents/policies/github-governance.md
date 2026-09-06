# Gobernanza y Seguridad en GitHub (Workspace ESP-IDF)

Este documento (Capa 2) define el estándar estricto de configuración de GitHub para todos los repositorios embebidos del workspace.

## 1. Plantillas y Archivos Base
Todo repositorio MUST incluir y mantener actualizados:
- `README.md`, `CHANGELOG.md`, `LICENSE`, `CONTRIBUTING.md`, `SECURITY.md`.
- `CODEOWNERS` (con revisión granular por rutas críticas).
- `.github/PULL_REQUEST_TEMPLATE.md` y `.github/ISSUE_TEMPLATE/` (o Issue Forms).
- `.github/dependabot.yml`.

## 2. GitHub Rulesets (Ramas Protegidas)
La rama principal protegida MUST gestionarse mediante **GitHub Rulesets** (no branch protection heredado). El ruleset MUST exigir:
- Pull Request obligatorio para merge.
- Prohibición de pushes directos, force-push y borrado de rama.
- Resolución obligatoria de todas las conversaciones.
- Revisión obligatoria delegada por `CODEOWNERS`.
- Dismiss stale approvals habilitado.
- Rama actualizada con base antes del merge.
- El repositorio SHOULD usar Squash Merge como estrategia por defecto.
- El título del Pull Request MUST seguir Conventional Commits cuando vaya a mergearse.

### Aprobaciones Mínimas
- **≥1 aprobación** para cambios regulares.
- **≥2 aprobaciones** para cambios en rutas críticas: seguridad, arranque (boot), energía, particiones, radio, protocolo o CI/CD.

### Status Checks Estrictos
El ruleset MUST requerir explícitamente los checks `format`, `lint`, `build`, `size`, `unit-tests` y `security`, con nombres estables y documentados en `CONTRIBUTING.md`.

## 3. Revisiones Especializadas (CODEOWNERS Granular)
Los cambios sobre las siguientes áreas MUST requerir review explícito de owners especializados:
- `.github/` (Infraestructura CI/CD).
- `docs/adr/` (Decisiones de arquitectura).
- `partitions*` (Gestión de memoria flash).
- Código fuente crítico: `boot`, cifrado de seguridad, gestión de energía (deep sleep) y stack de protocolo.

## 4. Hardening de GitHub Actions
Los workflows de GitHub Actions MUST seguir el principio de mínimo privilegio:
- Fijar permisos mínimos de `GITHUB_TOKEN` al inicio de cada workflow.
- Pinear (anclar) actions críticas por hash SHA cuando sea viable.
- Ejecutar validaciones inmutables de formato, build, tests, tamaño de flash y seguridad.

## 5. Seguridad de Supply Chain
- **CodeQL, Secret Scanning y Push Protection** MUST habilitarse en todos los repositorios que acepten contribuciones y manejen credenciales, tokens o configuraciones de despliegue.
- **Dependabot** MUST configurarse mediante `.github/dependabot.yml`, con revisión al menos semanal para `github-actions` y dependencias de tooling.

## 6. Releases
Las releases MUST:
- Usar tags estrictos SemVer.
- Adjuntar artefactos relevantes (binarios, SBOM).
- Incluir notas de versión generadas a partir del Changelog.
- Documentar explícitamente riesgos conocidos.
- Declarar matriz de compatibilidad de hardware y protocolo cuando aplique (sensor vs gateway).
