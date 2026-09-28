import QtQuick
import GenesisAppTest 1.0

// The surface under test is a Panel (a bare Rectangle with no intrinsic size),
// so the wrapper must give it the view's dimensions. Without this the ListView
// anchors to a zero-height parent and never instantiates a delegate.
ClipList {
    height: 720
    tracks: timelineModel.tracks
    width: 1280
}
