import QtQuick
import GenesisAppTest 1.0

// The surface under test is a Panel (a bare Rectangle with no intrinsic size),
// so the wrapper must give it the view's dimensions.
TransportBar {
    height: 720
    width: 1280
}
