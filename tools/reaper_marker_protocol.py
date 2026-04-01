SUBSCRIBE_MARKERS_COMMAND = "RS_SUBSCRIBE_MARKERS"


def format_marker_event_line(json_payload: str) -> str:
    return f"EVENT MARKERS {json_payload}"
