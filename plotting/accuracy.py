import matplotlib.pyplot as plt
import seaborn as sns
import pandas as pd
plt.rcParams.update({'font.size': 14})

DEBUG = False

precision_names = {11: "FP16", 24: "FP32", 53: "FP64"}
files_names = ["addition_combined", "multiplication_new"]
hue_order_addition = ["native", "HEAR0", "HEAR1", "HEAR2"]
hue_order_multiplication = ["Native", "HEAR"]

fig, ax = plt.subplots(2, 3, figsize=(9, 6))
fig.tight_layout()
fig.subplots_adjust(hspace=0.05)

for i, file_name in enumerate(files_names):
    for j, precision in enumerate(precision_names.keys()):
        operation = file_name.split("_")[0]
        dataframe = pd.read_csv(f"../tests/accuracy/results/{precision}_float_{file_name}.csv")
        dataframe["error"] = dataframe["error"].apply(float)
        dataframe["error"] = dataframe["error"][dataframe["error"] > 0]
        dataframe["precision"] = precision_names[precision]
        axis = ax[i][j]
        if operation == "addition":
            sns.boxplot(palette="tab10", x="precision", y="error", hue="type", hue_order=hue_order_addition,
                        data=dataframe, ax=axis)
        else:
            sns.boxplot(palette="tab10", x="precision", y="error", hue="type", hue_order=hue_order_multiplication,
                        data=dataframe, ax=axis)
        axis.set_yscale("log")
        axis.set_axisbelow(True)
        axis.grid(axis="y")
        axis.set_ylabel("")
        axis.set_xlabel("")
        if i != 0 or j != 0:
            axis.legend().remove()
        else:
            handles, labels = axis.get_legend_handles_labels()
            for index, (label, handle) in enumerate(zip(labels, handles)):
                if label == "native":
                    labels[index] = "Native"
                elif label == "HEAR0":
                    labels[index] = "HEAR $\gamma=2$"
                elif label == "HEAR1":
                    labels[index] = "HEAR $\gamma=1$"
                elif label == "HEAR2":
                    labels[index] = "HEAR $\gamma=0$"
            axis.legend(labels=labels, handles=handles, bbox_to_anchor=(0, 1.01, 1, 0.2), loc="lower left", ncol=4)
        if i == 0:
            axis.get_xaxis().set_visible(False)

fig.text(-0.03, 0.5, 'Relative error', va='center', rotation='vertical')
fig.text(0.98, 0.75, 'Addition', va='center', rotation='vertical')
fig.text(0.98, 0.30, 'Multiplication', va='center', rotation='vertical')
plt.savefig("figures/accuracy.pdf", bbox_inches='tight')
plt.show()
#
#
# for file in glob.glob(f"../tests/accuracy/results/*_float_{op}_{rnd}.csv"):
#     print(file)
#     precision = int(os.path.basename(file).split("_")[0])
#     dataframe = pd.read_csv(file)
#     if DEBUG:
#         dataframe.dropna()
#         for index, error in dataframe.iterrows():
#             if isinstance(error["error"], str):
#                 print(error)
#     dataframe["error"] = dataframe["error"].apply(float)
#     dataframe["error"] = dataframe["error"][dataframe["error"] > 0]
#     name = precision
#     if precision in precision_names:
#         name = precision_names[precision]
#     dataframe["precision"] = name
#     results = results.append(dataframe, ignore_index=True)
# if DEBUG:
#     print(results[results["error"] == 0])
#
# # Plot
# sns.set_theme(style="whitegrid", font_scale=2, rc={'figure.figsize': (11.7, 8.27), "ytick.left": True})
#
# # Tweak the visual presentation
# plot.legend().set_title(None)
# plot.set_yscale('log')
# plt.grid(True, which="minor", ls="--", c='gray')
# # current_values = plt.gca().get_yticks()
# # plt.gca().set_yticklabels(["$10^{"+f"{x:.0f}"+"}$" for x in current_values])
# plt.xlabel("")
# plt.ylabel("Relative error")
# plt.savefig(f"figures/{op}_{rnd}.pdf",  bbox_inches='tight')
# plt.show()
