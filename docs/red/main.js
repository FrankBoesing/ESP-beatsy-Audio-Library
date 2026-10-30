/** Modified from original Node-Red source, for audio system visualization
 * vim: set ts=4:
 * Copyright 2013 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/
var RED = (function() {

	$('#btn-keyboard-shortcuts').click(function(){showHelp();});

	function hideDropTarget() {
		$("#dropTarget").hide();
		RED.keyboard.remove(/* ESCAPE */ 27);
	}

	$('#chart').on("dragenter",function(event) {
		if ($.inArray("text/plain",event.originalEvent.dataTransfer.types) != -1) {
			$("#dropTarget").css({display:'table'});
			RED.keyboard.add(/* ESCAPE */ 27,hideDropTarget);
		}
	});

	$('#dropTarget').on("dragover",function(event) {
		if ($.inArray("text/plain",event.originalEvent.dataTransfer.types) != -1) {
			event.preventDefault();
		}
	})
	.on("dragleave",function(event) {
		hideDropTarget();
	})
	.on("drop",function(event) {
		var data = event.originalEvent.dataTransfer.getData("text/plain");
		hideDropTarget();
		RED.view.importNodes(data);
		event.preventDefault();
	});
	function cpp_base_name(n) {
		return (n._def && n._def.shortName) ? n._def.shortName : n.type.replace(/^Analog/, "");
	}

	function normalize_cpp_name(name) {
		return name.replace(" ", "_").replace("+", "_").replace("-", "_");
	}

	function is_auto_cpp_name(n) {
		var base = cpp_base_name(n);
		var name = n.name ? n.name : n.id;
		if (name !== n.id || name.substring(0, base.length) !== base) return false;
		var suffix = name.substring(base.length);
		if (/[0-9]$/.test(base)) {
			return suffix === "" || (suffix.charAt(0) === "_" && /^\d+$/.test(suffix.substring(1)));
		}
		return /^\d+$/.test(suffix);
	}

	function create_cpp_names(nns) {
		var names = Object.create(null);
		var used = Object.create(null);

		// Reserve names that were explicitly set by the user.
		for (var i = 0; i < nns.length; i++) {
			var n = RED.nodes.node(nns[i].id);
			if (n && !is_auto_cpp_name(n)) {
				var name = normalize_cpp_name(n.name ? n.name : n.id);
				names[n.id] = name;
				used[name] = true;
			}
		}

		// Rebuild default names from the node type, removing legacy suffixes
		// such as i2s2_1 when only one node uses the i2s2 basename.
		for (var i = 0; i < nns.length; i++) {
			var n = RED.nodes.node(nns[i].id);
			if (!n || Object.prototype.hasOwnProperty.call(names, n.id)) continue;

			var base = cpp_base_name(n);
			var hasNumericSuffix = /[0-9]$/.test(base);
			var count = hasNumericSuffix ? 0 : 1;
			var name = hasNumericSuffix ? base : base + count;
			while (used[name]) {
				count++;
				name = base + (hasNumericSuffix ? "_" : "") + count;
			}
			names[n.id] = name;
			used[name] = true;
		}
		return names;
	}

	function make_name(n, cppNames) {
		var name;
		if (cppNames && Object.prototype.hasOwnProperty.call(cppNames, n.id)) {
			name = cppNames[n.id];
		} else {
			name = normalize_cpp_name(n.name ? n.name : n.id);
		}
		return name;
	}

	// Preserve the original geometric order where it is still needed,
	// especially for stable automatic C++ names.
	function sort_nodes_by_position(nns) {
		var ordered = nns.slice();
		ordered.sort(function(a, b) {
			return (a.x + a.y / 250) - (b.x + b.y / 250);
		});
		return ordered;
	}

	// Return a stable topological ordering of the audio graph.
	// Nodes that are simultaneously ready retain the old geometric order.
	function sort_nodes_topologically(nns) {
		var orderedByPosition = sort_nodes_by_position(nns);

		var nodesById = Object.create(null);
		var indegree = Object.create(null);
		var outgoing = Object.create(null);
		var positionIndex = Object.create(null);

		for (var i = 0; i < orderedByPosition.length; i++) {
			var node = orderedByPosition[i];
			nodesById[node.id] = node;
			indegree[node.id] = 0;
			outgoing[node.id] = [];
			positionIndex[node.id] = i;
		}

		// Build the directed graph from source-node wires. Count every wire,
		// including parallel connections, so every edge is removed exactly once.
		for (var i = 0; i < orderedByPosition.length; i++) {
			var node = orderedByPosition[i];
			if (!node.wires) continue;

			for (var j = 0; j < node.wires.length; j++) {
				var wires = node.wires[j];
				if (!wires) continue;

				for (var k = 0; k < wires.length; k++) {
					var wire = wires[k];
					if (!wire) continue;

					var parts = wire.split(":");
					if (parts.length != 2 ||
						!Object.prototype.hasOwnProperty.call(nodesById, parts[0])) {
						continue;
					}

					outgoing[node.id].push(parts[0]);
					indegree[parts[0]]++;
				}
			}
		}

		// Kahn's algorithm. Keep the ready queue ordered by the previous
		// geometric order to make exports deterministic and minimize diffs.
		var ready = [];
		for (var i = 0; i < orderedByPosition.length; i++) {
			var id = orderedByPosition[i].id;
			if (indegree[id] === 0) ready.push(id);
		}

		var sorted = [];
		while (ready.length > 0) {
			var id = ready.shift();
			sorted.push(nodesById[id]);

			var targets = outgoing[id];
			for (var j = 0; j < targets.length; j++) {
				var targetId = targets[j];
				indegree[targetId]--;
				if (indegree[targetId] === 0) {
					ready.push(targetId);
					ready.sort(function(a, b) {
						return positionIndex[a] - positionIndex[b];
					});
				}
			}
		}

		// Feedback paths can contain cycles, for which a complete topological
		// ordering does not exist. Preserve export functionality by appending
		// the remaining nodes in their old geometric order and report the issue.
		if (sorted.length != orderedByPosition.length) {
			var sortedIds = Object.create(null);
			for (var i = 0; i < sorted.length; i++) {
				sortedIds[sorted[i].id] = true;
			}

			var remaining = [];
			for (var i = 0; i < orderedByPosition.length; i++) {
				var node = orderedByPosition[i];
				if (!Object.prototype.hasOwnProperty.call(sortedIds, node.id)) {
					remaining.push(node);
				}
			}

			if (typeof console !== "undefined" && console.warn) {
				console.warn(
					"Audio graph contains a feedback cycle. A complete topological " +
					"export order is not possible; remaining nodes use geometric order.",
					remaining.map(function(node) { return node.name || node.id; })
				);
			}

			sorted = sorted.concat(remaining);
		}

		return sorted;
	}

	function save(force) {
		RED.storage.update();

		if (RED.nodes.hasIO()) {
			var nodeSet = RED.nodes.createCompleteNodeSet();
			var cppNames = create_cpp_names(sort_nodes_by_position(nodeSet));
			var nns = sort_nodes_topologically(nodeSet);
			//console.log(JSON.stringify(nns));

			var usbInputNodes = nns.filter(function(n) { return n.type === "AudioInputUSB"; });
			var usbOutputNodes = nns.filter(function(n) { return n.type === "AudioOutputUSB"; });
			var hasUsbInput = usbInputNodes.length > 0;
			var hasUsbOutput = usbOutputNodes.length > 0;
			var hasUsbAudio = hasUsbInput || hasUsbOutput;
			var cpp = "#include <Audio.h>\n";
if (hasUsbAudio) {
	cpp += "/*\n"
		+ " * USB Audio is managed by the library. Default AudioInputUSB / AudioOutputUSB\n"
		+ " * constructors select the required speaker/microphone endpoints.\n"
		+ " * AudioMemory() starts the shared USB Audio + CDC device and routes stream\n"
		+ " * enable events. No USB includes, USBAudioCard object, USB.begin() or custom\n"
		+ " * event callback are needed in this managed mode.\n"
		+ " * AUDIO_USB_REDIRECT_SERIAL maps Serial to managed CDC; use Serial0 for UART0.\n"
		+ " * Add -D AUDIO_USB_REDIRECT_SERIAL to PlatformIO build_src_flags for the sketch.\n"
		+ " * Runtime ESP-IDF logs are routed to CDC while the host terminal is connected.\n"
		+ " * Set AUDIO_SAMPLE_RATE_EXACT to the intended USB rate before compiling.\n"
		+ " * The framework must enable CONFIG_TINYUSB_AUDIO_ENABLED.\n"
		+ " * For the tested Windows CDC + UAC1 composite on Arduino-ESP32 3.3.12,\n"
		+ " * this repository's version-specific workaround may be needed:\n"
		+ " *   extra_scripts = pre:scripts/patch_uac1_audio_iad.py\n"
		+ " */\n\n";
} else {
	cpp += "\n";
}
cpp += "// GUItool: begin automatically generated code\n";
			// generate code for all audio processing nodes
			for (var i=0; i<nns.length; i++) {
				var n = nns[i];
				var node = RED.nodes.node(n.id);
				if (node && (node.outputs > 0 || node._def.inputs > 0)) {
					cpp += n.type + " ";
					for (var j=n.type.length; j<24; j++) cpp += " ";
					var name = make_name(n, cppNames);
					// AudioMixerN's constructor must match the input count shown by this node.
					var constructorArgs = "";
					if (n.type === "AudioMixerN") {
						var mixerChannels = parseInt(n.channels, 10);
						if (isNaN(mixerChannels)) mixerChannels = 32;
						mixerChannels = Math.max(4, Math.min(32, mixerChannels));
						constructorArgs = "(" + mixerChannels + ")";
					}
					cpp += name + constructorArgs + "; ";
					for (var j=n.id.length; j<14; j++) cpp += " ";
					cpp += "//xy=" + n.x + "," + n.y + "\n";
				}
			}
			cpp += "\n";
			
			// generate code for all connections (aka wires or links)
			var cordcount = 1;
			for (var i=0; i<nns.length; i++) {
				var n = nns[i];
				if (n.wires) {
					for (var j=0; j<n.wires.length; j++) {
						var wires = n.wires[j];
						if (!wires) continue;
						for (var k=0; k<wires.length; k++) {
							var wire = n.wires[j][k];
							if (wire) {
								var parts = wire.split(":");
								if (parts.length == 2) {
									cpp += "AudioConnection          patchCord" + cordcount + "(";
									var src = RED.nodes.node(n.id);
									var dst = RED.nodes.node(parts[0]);
									var src_name = make_name(src, cppNames);
									var dst_name = make_name(dst, cppNames);
									if (j == 0 && parts[1] == 0 && src && src.outputs == 1 && dst && dst._def.inputs == 1) {
										cpp += src_name + ", " + dst_name;
									} else {
										cpp += src_name + ", " + j + ", " + dst_name + ", " + parts[1];
									}
									cpp += ");\n";
									cordcount++;
								}
							}
						}
					}
				}
			}
			cpp += "\n";
			
			// generate code for all control nodes (no inputs or outputs)
			for (var i=0; i<nns.length; i++) {
				var n = nns[i];
				var node = RED.nodes.node(n.id);
				if (node && node.outputs == 0 && node._def.inputs == 0) {
					cpp += n.type + " ";
					for (var j=n.type.length; j<24; j++) cpp += " ";
					cpp += make_name(n, cppNames) + "; ";
					for (var j=n.id.length; j<14; j++) cpp += " ";
					cpp += "//xy=" + n.x + "," + n.y + "\n";
				}
			}
			cpp += "// GUItool: end automatically generated code\n";
			//console.log(cpp);

			RED.view.state(RED.state.EXPORT);
			RED.view.getForm('dialog-form', 'export-clipboard-dialog', function (d, f) {
				$("#node-input-export").val(cpp).focus(function() {
				var textarea = $(this);
				textarea.select();
				textarea.mouseup(function() {
					textarea.unbind("mouseup");
					return false;
				});
				}).focus();
			$( "#dialog" ).dialog("option","title","Export to Arduino").dialog( "open" );
			});
			//RED.view.dirty(false);
		} else {
			$( "#node-dialog-error-deploy" ).dialog({
				title: "Error exporting data to Arduino IDE",
				modal: true,
				autoOpen: false,
				width: 410,
				height: 245,
				buttons: [{
					text: "Ok",
					click: function() {
						$( this ).dialog( "close" );
					}
				}]
			}).dialog("open");
		}
	}

	$('#btn-deploy').click(function() { save(); });


	$( "#node-dialog-confirm-deploy" ).dialog({
			title: "Confirm deploy",
			modal: true,
			autoOpen: false,
			width: 530,
			height: 230,
			buttons: [
				{
					text: "Confirm deploy",
					click: function() {
						save(true);
						$( this ).dialog( "close" );
					}
				},
				{
					text: "Cancel",
					click: function() {
						$( this ).dialog( "close" );
					}
				}
			]
	});

	// from http://css-tricks.com/snippets/javascript/get-url-variables/
	function getQueryVariable(variable) {
		var query = window.location.search.substring(1);
		var vars = query.split("&");
		for (var i=0;i<vars.length;i++) {
			var pair = vars[i].split("=");
			if(pair[0] == variable){return pair[1];}
		}
		return(false);
	}

	function loadNodes() {
			$(".palette-scroll").show();
			$("#palette-search").show();
			RED.storage.load();
			RED.view.redraw();
			setTimeout(function() {
				$("#btn-deploy").removeClass("disabled").addClass("btn-danger");
				$("#btn-import").removeClass("disabled").addClass("btn-success");
			}, 1500);
			$('#btn-deploy').click(function() { save(); });
			// if the query string has ?info=className, populate info tab
			var info = getQueryVariable("info");
			if (info) {
				RED.sidebar.info.setHelpContent('', info);
			}
	}

	$('#btn-node-status').click(function() {toggleStatus();});

	var statusEnabled = false;
	function toggleStatus() {
		var btnStatus = $("#btn-node-status");
		statusEnabled = btnStatus.toggleClass("active").hasClass("active");
		RED.view.status(statusEnabled);
	}
	
	function showHelp() {

		var dialog = $('#node-help');

		//$("#node-help").draggable({
		//        handle: ".modal-header"
		//});

		dialog.on('show',function() {
			RED.keyboard.disable();
		});
		dialog.on('hidden',function() {
			RED.keyboard.enable();
		});

		dialog.modal();
	}

	$(function() {
		$(".palette-spinner").show();

		// server test switched off - test purposes only
		var patt = new RegExp(/^[http|https]/);
		var server = false && patt.test(location.protocol);

		if (!server) {
			
			var metaData = $.parseJSON($("script[data-container-name|='InputOutputCompatibilityMetadata']").html());
			requirements = metaData["requirements"];
			
			var data = $.parseJSON($("script[data-container-name|='NodeDefinitions']").html());
			var nodes = data["nodes"];
			$.each(nodes, function (key, val) {
				RED.nodes.registerType(val["type"], val["data"]);
			});
			RED.keyboard.add(/* ? */ 191, {shift: true}, function () {
				showHelp();
				d3.event.preventDefault();
			});
			loadNodes();
			$(".palette-spinner").hide();
		} else {
			$.ajaxSetup({beforeSend: function(xhr){
				if (xhr.overrideMimeType) {
					xhr.overrideMimeType("application/json");
				}
			}});
			$.getJSON( "resources/nodes_def.json", function( data ) {
				var nodes = data["nodes"];
				$.each(nodes, function(key, val) {
					RED.nodes.registerType(val["type"], val["data"]);
				});
				RED.keyboard.add(/* ? */ 191,{shift:true},function(){showHelp();d3.event.preventDefault();});
				loadNodes();
				$(".palette-spinner").hide();
			})
		}
	});

	return {
	};
})();
