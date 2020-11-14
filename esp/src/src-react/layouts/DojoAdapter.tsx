import * as React from "react";
import * as ReactDOM from "react-dom";
import { useId } from "@fluentui/react-hooks";
import * as registry from "dijit/registry";
import nlsHPCC from "src/nlsHPCC";
import { resolve } from "src/Utility";

export interface DojoProps {
    widgetClassID?: string;
    widgetClass?: any;
    params?: object;
    onWidgetMount?: (widget) => void;
}

export interface DojoState {
    uid: number;
    widgetClassID?: string;
    widget: any;
}

export const DojoAdapter: React.FunctionComponent<DojoProps> = ({
    widgetClassID,
    widgetClass,
    params,
    onWidgetMount
}) => {

    const myRef = React.useRef<HTMLDivElement>();
    const uid = useId("");

    React.useEffect(() => {

        const elem = document.createElement("div");
        myRef.current.innerText = "";
        myRef.current.appendChild(elem);

        let widget = undefined;

        if (widgetClassID) {
            resolve(widgetClassID, widgetClass => {
                init(widgetClass);
            });
        } else if (widgetClass) {
            init(widgetClass);
        }

        function init(WidgetClass) {
            if (widget === undefined) { //  Test for race condition  --
                widget = new WidgetClass({
                    id: `dojo-component-widget-${uid}`,
                    style: {
                        margin: "0px",
                        padding: "0px",
                        width: "100%",
                        height: "100%"
                    }
                }, elem);
                // widget.placeAt(elem, "replace");
                widget.startup();
                widget.resize();
                if (widget.init) {
                    widget.init(params || {});
                }

                if (onWidgetMount) {
                    onWidgetMount(widget);
                }
            }
        }

        return () => {
            if (widget) {
                widget.destroyRecursive(true);

                //  Should not be needed  ---
                registry.toArray().filter(w => w.id.indexOf(`dojo-component-widget-${uid}`) === 0).forEach(w => {
                    w.destroyRecursive(true);
                });
                //  ---

                const domNode = ReactDOM.findDOMNode(myRef.current) as Element;
                domNode.innerHTML = "";
            }
            widget = null;  //  Avoid race condition  ---
        };
    }, [widgetClassID]);

    return <div ref={myRef} style={{ width: "100%", height: "100%" }}>{nlsHPCC.Loading} {widgetClassID}...</div>;
};
