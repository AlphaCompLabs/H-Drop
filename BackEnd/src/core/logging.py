import logging
import sys


def configurar_logging(debug: bool = False) -> None:
    nivel = logging.DEBUG if debug else logging.INFO
    formato = "[%(asctime)s.%(msecs)03d] %(levelname)-5s %(name)s | %(message)s"
    logging.basicConfig(
        level=nivel,
        format=formato,
        datefmt="%H:%M:%S",
        stream=sys.stdout,
        force=True,
    )
    logging.getLogger("aiomqtt").setLevel(logging.WARNING)
    logging.getLogger("uvicorn.access").setLevel(logging.WARNING)


def get_logger(nome: str) -> logging.Logger:
    return logging.getLogger(nome)
