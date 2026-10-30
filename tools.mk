# Python development checks do not require configuring the C toolchain.
UV = uv

fmt:
	"$(UV)" run --locked ruff format .

check-format:
	"$(UV)" run --locked ruff format --check .

lint: check-format
	"$(UV)" run --locked ruff check .
	"$(UV)" run --locked ty check --error-on-warning .
